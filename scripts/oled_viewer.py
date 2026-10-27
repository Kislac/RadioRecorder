"""Show the ESP OLED framebuffer and drive its encoder over COM3 (or another port)."""

import argparse
import queue
import threading
import time

WIDTH = 128
HEIGHT = 64
FRAME_BYTES = WIDTH * HEIGHT // 8


def parse_frame(line):
    parts = line.strip().split(b" ", 3)
    if len(parts) != 4 or parts[:3] != [b"OLED_FRAME", b"128", b"64"]:
        raise ValueError("Not an OLED frame")
    if len(parts[3]) != FRAME_BYTES * 2:
        raise ValueError("Incorrect OLED frame length")
    try:
        return bytes.fromhex(parts[3].decode("ascii"))
    except (UnicodeError, ValueError) as error:
        raise ValueError("Invalid OLED frame data") from error


def pixel(frame, x, y):
    return bool(frame[(y // 8) * WIDTH + x] & (1 << (y % 8)))


def body_pixels(frame):
    return tuple(pixel(frame, x, y) for y in range(11, 56) for x in range(WIDTH))


def open_device(port):
    import serial

    device = serial.Serial(baudrate=115200, timeout=1, write_timeout=1)
    device.port = port
    device.dtr = False
    device.rts = False
    device.open()
    device.reset_input_buffer()
    return device


def read_frame(device):
    device.write(b"d")
    deadline = time.monotonic() + 2.5
    while time.monotonic() < deadline:
        line = device.readline()
        if line.startswith(b"OLED_FRAME "):
            return parse_frame(line)
        if line.startswith(b"OLED_ERROR "):
            raise RuntimeError(line.decode("ascii", errors="replace").strip())
    raise TimeoutError("No OLED frame received; is the test firmware installed?")


def main():
    import tkinter as tk
    from tkinter import filedialog, messagebox

    parser = argparse.ArgumentParser(description="Live 128x64 OLED viewer over serial")
    parser.add_argument("--port", default="COM3", help="ESP serial port (default: COM3)")
    args = parser.parse_args()
    root = tk.Tk()
    root.title("RadioRecorder OLED - " + args.port)
    root.resizable(False, False)
    canvas = tk.Canvas(root, width=WIDTH * 5, height=HEIGHT * 5, bg="#141a19", highlightthickness=0)
    canvas.pack(padx=12, pady=(12, 4))
    image_id = canvas.create_image(0, 0, anchor="nw")
    commands = queue.Queue()
    updates = queue.Queue(maxsize=2)
    stopped = threading.Event()
    last_frame = None

    def worker():
        try:
            with open_device(args.port) as device:
                while not stopped.is_set():
                    while not commands.empty():
                        device.write(commands.get_nowait().encode("ascii"))
                    frame = read_frame(device)
                    if updates.full():
                        updates.get_nowait()
                    updates.put((frame, None))
        except Exception as error:
            updates.put((None, str(error)))

    def refresh():
        nonlocal last_frame
        while not updates.empty():
            frame, error = updates.get_nowait()
            if error:
                status.config(text=error)
                messagebox.showerror("OLED", error)
                return
            last_frame = frame
            image = tk.PhotoImage(width=WIDTH, height=HEIGHT)
            for y in range(HEIGHT):
                colors = ("#c6f173" if pixel(frame, x, y) else "#141a19" for x in range(WIDTH))
                image.put("{" + " ".join(colors) + "}", to=(0, y))
            canvas.image = image.zoom(5, 5)
            canvas.itemconfigure(image_id, image=canvas.image)
            status.config(text="OLED 128 x 64  |  " + args.port)
        root.after(80, refresh)

    def save_frame():
        if last_frame is None:
            return
        path = filedialog.asksaveasfilename(defaultextension=".pbm", filetypes=[("PBM image", "*.pbm")])
        if path:
            with open(path, "wb") as output:
                output.write(b"P4\n128 64\n")
                for y in range(HEIGHT):
                    for x in range(0, WIDTH, 8):
                        output.write(bytes([sum(1 << (7 - index) for index in range(8)
                                                if pixel(last_frame, x + index, y))]))

    toolbar = tk.Frame(root)
    toolbar.pack(pady=(4, 8))
    for label, command in (("Balra", "j"), ("Jobbra", "l"), ("Nyomás", "k"), ("Hosszú", "b")):
        tk.Button(toolbar, text=label, command=lambda value=command: commands.put(value)).pack(side="left", padx=4)
    tk.Button(toolbar, text="Kép mentése", command=save_frame).pack(side="left", padx=4)
    status = tk.Label(root, text="Kapcsolódás...", anchor="w")
    status.pack(fill="x", padx=12, pady=(0, 10))
    for key, command in (("<Left>", "j"), ("<Right>", "l"), ("<Return>", "k"), ("<Escape>", "b")):
        root.bind(key, lambda event, value=command: commands.put(value))
    root.protocol("WM_DELETE_WINDOW", lambda: (stopped.set(), root.destroy()))
    threading.Thread(target=worker, daemon=True).start()
    root.after(80, refresh)
    root.mainloop()


if __name__ == "__main__":
    main()