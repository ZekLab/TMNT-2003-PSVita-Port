"""Window for a port's PC step: choose the game folder and where the Vita files go, press Build. Shared by the ports; each port
gives a Port (name, the folder on the Vita, the build function). Nothing of the game leaves the computer."""
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import traceback
import tkinter as tk
from tkinter import filedialog, messagebox, ttk


class Port:
    def __init__(self, name, vita_folder, out_name, game_hint, build):
        self.name = name                # shown in the title
        self.vita_folder = vita_folder  # where the files go on the Vita, e.g. ux0:data/TMNT2003/
        self.out_name = out_name        # default output folder name (on the desktop)
        self.game_hint = game_hint      # what the game folder must hold
        self.build = build              # build(game, out, work, log, progress); raises an exception with a clear message


def open_folder(path):
    if sys.platform == "darwin":
        subprocess.Popen(["open", path])
    elif os.name == "nt":
        os.startfile(path)
    else:
        subprocess.Popen(["xdg-open", path])


class BuilderApp:
    def __init__(self, port):
        self.port = port
        self.events = queue.Queue()   # from the build thread: ("log", text), ("progress", fraction, text), ("done", error or None)
        self.busy = False
        root = self.root = tk.Tk()
        root.title("%s - Vita files builder" % port.name)
        root.minsize(640, 460)
        frame = ttk.Frame(root, padding=14)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)
        frame.rowconfigure(6, weight=1)

        intro = ("Makes the files the Vita needs from your copy of %s. Choose %s, then press Build.\n"
                 "Nothing of the game leaves your computer." % (port.name, port.game_hint))
        ttk.Label(frame, text=intro, wraplength=600, justify="left").grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 10))

        self.game = tk.StringVar()
        self.out = tk.StringVar(value=os.path.join(os.path.expanduser("~"), "Desktop", port.out_name))
        for row, (label, var) in enumerate((("Game folder", self.game), ("Output folder", self.out)), start=1):
            ttk.Label(frame, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=3)
            ttk.Entry(frame, textvariable=var).grid(row=row, column=1, sticky="ew", pady=3)
            ttk.Button(frame, text="Browse...", command=lambda v=var: self.browse(v)).grid(row=row, column=2, padx=(8, 0), pady=3)

        self.build_button = ttk.Button(frame, text="Build", command=self.start)
        self.build_button.grid(row=3, column=0, columnspan=3, pady=(10, 6))
        self.bar = ttk.Progressbar(frame, maximum=1000)
        self.bar.grid(row=4, column=0, columnspan=3, sticky="ew")
        self.status = tk.StringVar(value="Ready.")
        ttk.Label(frame, textvariable=self.status).grid(row=5, column=0, columnspan=3, sticky="w", pady=(4, 6))

        logbox = ttk.Frame(frame)
        logbox.grid(row=6, column=0, columnspan=3, sticky="nsew")
        logbox.columnconfigure(0, weight=1)
        logbox.rowconfigure(0, weight=1)
        self.log = tk.Text(logbox, height=12, wrap="word", state="disabled")
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(logbox, command=self.log.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.log["yscrollcommand"] = scroll.set

        self.open_button = ttk.Button(frame, text="Open the output folder", command=lambda: open_folder(self.out.get()), state="disabled")
        self.open_button.grid(row=7, column=0, columnspan=3, pady=(8, 0))
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.poll)

    def browse(self, var):
        path = filedialog.askdirectory(initialdir=var.get() or os.path.expanduser("~"))
        if path:
            var.set(path)

    def add_log(self, text):
        self.log["state"] = "normal"
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log["state"] = "disabled"

    def start(self):
        game, out = self.game.get().strip(), self.out.get().strip()
        if not game or not os.path.isdir(game):
            messagebox.showerror("Build", "Choose the game folder first (%s)." % self.port.game_hint)
            return
        if not out:
            messagebox.showerror("Build", "Choose the output folder.")
            return
        self.busy = True
        self.build_button["state"] = self.open_button["state"] = "disabled"
        self.log["state"] = "normal"
        self.log.delete("1.0", "end")
        self.log["state"] = "disabled"
        threading.Thread(target=self.run, args=(os.path.abspath(game), os.path.abspath(out)), daemon=True).start()

    def run(self, game, out):
        work = tempfile.mkdtemp(prefix="vita_builder_")
        logfile = None
        try:
            os.makedirs(out, exist_ok=True)
            logfile = open(os.path.join(out, "builder_log.txt"), "w", encoding="utf-8")   # to share when something fails
            logfile.write("%s builder, game folder %s\n" % (self.port.name, game))

            def log(text):
                logfile.write(str(text) + "\n")
                logfile.flush()
                self.events.put(("log", str(text)))

            self.port.build(game, out, work, log, lambda fraction, text: self.events.put(("progress", fraction, text)))
            self.events.put(("done", None))
        except Exception as e:   # the port's error message, shown as it is
            if logfile:
                logfile.write("\nError: %s\n%s" % (e, traceback.format_exc()))
            self.events.put(("done", str(e) or e.__class__.__name__))
        finally:
            if logfile:
                logfile.close()
            shutil.rmtree(work, ignore_errors=True)

    def poll(self):
        try:
            while True:
                event = self.events.get_nowait()
                if event[0] == "log":
                    self.add_log(event[1])
                elif event[0] == "progress":
                    self.bar["value"] = int(event[1] * 1000)
                    self.status.set(event[2])
                else:
                    self.finish(event[1])
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def finish(self, error):
        self.busy = False
        self.build_button["state"] = "normal"
        if error:
            self.status.set("Failed.")
            self.add_log("\nError: " + error)
            messagebox.showerror("Build failed", error)
            return
        self.open_button["state"] = "normal"
        done = "Done. Copy everything in the output folder to %s on the Vita." % self.port.vita_folder
        self.status.set(done)
        messagebox.showinfo("Build", done)

    def close(self):
        if self.busy and not messagebox.askyesno("Build", "A build is running. Quit anyway?"):
            return
        self.root.destroy()

    def run_app(self, game=None, out=None):
        """game and out (tests): filled in and the build started."""
        if game:
            self.game.set(game)
        if out:
            self.out.set(out)
        if game and out:
            self.root.after(500, self.start)
        self.root.mainloop()
