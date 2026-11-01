import unittest
from pathlib import Path
from xml.etree import ElementTree


WIRING = Path(__file__).resolve().parents[1] / "docs" / "wiring"


class IsolatedAudioWiringTests(unittest.TestCase):
    def diagram(self, name):
        root = ElementTree.parse(WIRING / name).getroot()
        parts = {element.get("id") for element in root.iter() if element.get("id")}
        labels = " ".join(element.text or "" for element in root.iter())
        return root, parts, labels

    def test_receive_dc_block_and_separate_returns(self):
        root, parts, labels = self.diagram("radio-receive-isolated.svg")
        self.assertIn("rx-dc-block", parts)
        self.assertIn("rx-radio-winding", parts)
        self.assertIn("rx-esp-winding", parts)
        self.assertIn("rx-esp-return", parts)
        self.assertIn("rx-adc-attenuator", parts)
        self.assertIn("SPK-?", labels)
        self.assertIn("ESP GND ONLY", labels)
        self.assertIn("R1 10 kohm", labels)
        self.assertEqual(root.get("viewBox"), "0 0 1400 740")

    def test_transmit_dac_ground_mic_bias_block_and_separate_ptt(self):
        _, parts, labels = self.diagram("radio-transmit-isolated.svg")
        self.assertIn("tx-series-resistor", parts)
        self.assertIn("tx-dac-return", parts)
        self.assertIn("tx-radio-return", parts)
        self.assertIn("tx-mic-dc-block", parts)
        self.assertIn("tx-ptt-contact", parts)
        self.assertIn("always connected", labels)
        self.assertIn("NO, open at rest", labels)
        self.assertIn("Do not connect PCM5102A GND to radio MIC-", labels)

    def test_receive_and_transmit_diagrams_are_linked(self):
        docs = (WIRING.parent / "functional-hardware.md").read_text(encoding="utf-8")
        for diagram in ("radio-receive-isolated.svg", "radio-transmit-isolated.svg"):
            self.assertIn("(wiring/" + diagram + ")", docs)
            self.assertTrue((WIRING / diagram).is_file())

    def test_retired_wiring_diagrams_are_not_published(self):
        documents = [WIRING.parent.parent / "README.md", WIRING.parent / "functional-hardware.md",
                     WIRING.parent / "UV5R audio interface.md"]
        for name in ("radio-receive-adc.svg", "radio-pdm-output.svg"):
            self.assertFalse((WIRING / name).exists())
            for document in documents:
                with self.subTest(diagram=name, document=document.name):
                    self.assertNotIn(name, document.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()