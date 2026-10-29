import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]

ARDUINO = r"""
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
using String = std::string;
extern uint32_t testNow;
inline uint32_t millis() { return testNow; }
#define SERIAL_8N1 0
"""

UART = r"""
#pragma once
#include <deque>
#include <vector>
#include <cassert>
class HardwareSerial {
public:
  bool enabled = false;
  std::deque<uint8_t> incoming;
  std::vector<std::vector<uint8_t>> transmissions;
  explicit HardwareSerial(int port) { assert(port == 1); }
  void begin(int baud, int, int rx, int tx) {
    assert(baud == 9600 && rx == 16 && tx == 17);
    enabled = true;
  }
  void end() { enabled = false; }
  int available() { return static_cast<int>(incoming.size()); }
  int read() { int value = incoming.front(); incoming.pop_front(); return value; }
  void write(uint8_t value) { write(&value, 1); }
  void write(const uint8_t *bytes, size_t length) {
    assert(enabled);
    transmissions.emplace_back(bytes, bytes + length);
    if (length > 1) assert(length == 4 && bytes[0] == 0x53);
  }
};
"""

JSON = r"""
#pragma once
#include <map>
#include <cstdlib>
struct cJSON {};
static std::map<std::string, double> numbers;
inline cJSON *cJSON_CreateObject() { return new cJSON; }
inline cJSON *cJSON_AddObjectToObject(cJSON *, const char *) { return new cJSON; }
inline cJSON *cJSON_AddArrayToObject(cJSON *, const char *) { return new cJSON; }
inline void cJSON_AddNumberToObject(cJSON *, const char *name, double value) { numbers[name] = value; }
inline void cJSON_AddStringToObject(cJSON *, const char *, const char *) {}
inline void cJSON_AddNullToObject(cJSON *, const char *) {}
inline void cJSON_AddBoolToObject(cJSON *, const char *, bool) {}
inline void cJSON_AddItemToArray(cJSON *, cJSON *item) { delete item; }
inline char *cJSON_PrintUnformatted(cJSON *) { return strdup("{}"); }
inline void cJSON_free(void *memory) { free(memory); }
inline void cJSON_Delete(cJSON *item) { delete item; }
"""

HARNESS = r"""
#include <iostream>
#include <algorithm>
#include <deque>
#include <vector>
uint32_t testNow = 0;
bool pttActive = false;
bool pttTestActive() { return pttActive; }
#include "radio_programming.cpp"

int main(int argc, char **argv) {
  assert(argc == 2);
  std::string scenario = argv[1];
  assert(!radioProgrammingBusy() && !radioSerial.enabled);
  assert(radioSerial.transmissions.empty());
  size_t length = 123;
  assert(!radioProgrammingImage(length) && length == 0);
  pttActive = true;
  assert(!radioProgrammingStart() && !radioSerial.enabled);
  pttActive = false;
  if (scenario == "wrap") testNow = 0xFFFFF000;
  assert(radioProgrammingStart());
  assert(!radioProgrammingStart());
  std::vector<uint8_t> memory(0x2000, 0xFF);
  uint8_t channel[] = {0x00,0x00,0x55,0x14,0x00,0x00,0x55,0x14,0,0,0x75,0x03,0,0,1,0x44};
  std::copy(channel, channel + 16, memory.begin());
  const char name[] = "TEST";
  std::copy(name, name + 4, memory.begin() + 0x1000);
  const char firmware[] = "BFB297 TEST1234";
  std::copy(firmware, firmware + 14, memory.begin() + 0x1EF0);
  memory[0x1FCF] = scenario == "dropped" ? 0xFF : 0x42;
  memory[0xE20] = 3;
  memory[0xE76] = 7;
  uint8_t vfo[] = {1,4,5,5,0,0,0,0};
  std::copy(vfo, vfo + 8, memory.begin() + 0xF08);
  std::deque<uint8_t> pending;
  size_t serviced = 0;
  std::vector<uint16_t> addresses;
  std::vector<uint8_t> sizes;
  for (uint32_t tick = 0; tick < 7000 && radioProgrammingBusy(); ++tick) {
    size_t chunk = scenario == "fragmented" ? 3 : 80;
    while (chunk-- && !pending.empty()) {
      radioSerial.incoming.push_back(pending.front());
      pending.pop_front();
    }
    radioProgrammingPoll();
    if (serviced != radioSerial.transmissions.size()) {
      serviced = radioSerial.transmissions.size();
      if (phase == Phase::magicAck) {
        if (scenario != "silent" && !(scenario == "legacy" && magicIndex == 0)) pending.push_back(0x06);
      } else if (phase == Phase::ident) {
        std::vector<uint8_t> ident = {0xAA,0x01,0x02,0x03,0x04,0x05,0x06,0xDD};
        if (scenario == "ident12") ident = {0xAA,1,1,2,1,3,1,4,5,6,7,0xDD};
        if (scenario == "badident") ident = {1,2,3,0xDD};
        pending.insert(pending.end(), ident.begin(), ident.end());
      } else if (phase == Phase::identAck) {
        pending.push_back(scenario == "refused" ? 0 : 0x06);
      } else if (phase == Phase::block) {
        addresses.push_back(address);
        sizes.push_back(blockSize);
        if (!firstBlock) pending.push_back(scenario == "badack" ? 0 : 0x06);
        pending.push_back(scenario == "badheader" ? 0x59 : 0x58);
        pending.push_back(address >> 8);
        pending.push_back(address & 255);
        pending.push_back(scenario == "badsize" ? 16 : blockSize);
        for (size_t index = 0; index < blockSize; ++index) pending.push_back(memory[address + index]);
        if (scenario == "short") pending.pop_back();
      }
    }
    testNow += 10;
  }
  assert(!radioProgrammingBusy() && !radioSerial.enabled);
  const uint8_t *dump = radioProgrammingImage(length);
  bool expectedFailure = scenario == "silent" || scenario == "badident" || scenario == "refused" ||
                         scenario == "badheader" || scenario == "badsize" || scenario == "badack" || scenario == "short";
  if (expectedFailure) {
    assert(phase == Phase::failed && failure[0]);
    assert(!dump && length == 0);
  } else {
    assert(phase == Phase::done && dump);
    assert(length == (scenario == "legacy" ? 0x1808 : 0x1948));
    assert(std::equal(memory.begin(), memory.begin() + 0x1800, dump + 8));
    if (scenario != "legacy") {
      assert(addresses[0] == 0x1E80 && addresses[1] == 0x1EC0 && addresses[2] == 0x1FC0 && addresses[3] == 0);
      assert(std::equal(memory.begin() + 0x1EC0, memory.end(), dump + 0x1808));
      assert(addresses.back() == (scenario == "dropped" ? 0x1FF0 : 0x1FC0));
      assert(sizes.back() == (scenario == "dropped" ? 16 : 64));
    } else assert(addresses.front() == 0 && addresses.back() == 0x17C0);
    uint32_t hz = 0;
    assert(frequency(dump + 8, hz) && hz == 145500000);
    uint8_t invalid[] = {0xFA,0,0,0};
    assert(!frequency(invalid, hz));
    radioProgrammingJson();
    assert(numbers["rxHz"] == 145500000 && numbers["txHz"] == 145500000);
    assert(numbers["txToneRaw"] == 885 && numbers["powerRaw"] == 1);
    assert(numbers["squelch"] == 3 && numbers["channelA"] == 7);
    assert(numbers["vfoAHz"] == 145500000);
    assert(numbers["bytesRead"] == (scenario == "legacy" ? 6144 : 6464));
  }
  assert(radioProgrammingStart());
  assert(!radioProgrammingImage(length) && length == 0);
  std::cout << "PASS " << scenario << std::endl;
}
"""


class RadioProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        fallback = Path.home() / ".platformio/packages/toolchain-gccmingw32/bin/g++.exe"
        if compiler is None and fallback.is_file():
            compiler = str(fallback)
        if compiler is None:
            raise unittest.SkipTest("Host g++ required: pio pkg install -g -t platformio/toolchain-gccmingw32")
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        (directory / "driver").mkdir()
        for name, content in {"Arduino.h": ARDUINO, "HardwareSerial.h": UART, "cJSON.h": JSON,
                              "driver/gpio.h": "#pragma once\n#define GPIO_NUM_16 16\n#define GPIO_NUM_17 17\ninline void gpio_reset_pin(int) {}\n",
                              "harness.cpp": '#include "Arduino.h"\n' + HARNESS}.items():
            (directory / name).write_text(content, encoding="utf-8")
        cls.executable = directory / ("radio-test.exe" if os.name == "nt" else "radio-test")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(directory),
                        "-I", str(ROOT / "include"), "-I", str(ROOT / "src"),
                        str(directory / "harness.cpp"), "-o", str(cls.executable)],
                       check=True, capture_output=True, text=True, env=cls.environment)

    def test_actual_firmware_uart_state_machine(self):
        for scenario in ("modern", "legacy", "ident12", "fragmented", "dropped", "wrap", "silent",
                         "badident", "refused", "badheader", "badsize", "badack", "short"):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.executable), scenario], capture_output=True, text=True,
                                        timeout=5, env=self.environment)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("PASS " + scenario, result.stdout)


if __name__ == "__main__":
    unittest.main()