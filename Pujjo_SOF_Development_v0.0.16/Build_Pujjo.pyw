# Pujjo/Pujjo1e software-only development GUI. NOT a driver installer.
# SPDX-License-Identifier: BSD-3-Clause
import os
import queue
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext
from pathlib import Path

ROOT = Path(__file__).resolve().parent
KIT = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Windows Kits' / '10'
LOG_DIR = ROOT / 'build-logs'
# Explicit offline allowlist includes test_kmdf_security.py and all portable suites.
TESTS = ['tests/run_offline_suite.py']

def python_console():
    exe = Path(sys.executable)
    if exe.name.lower() == 'pythonw.exe':
        candidate = exe.with_name('python.exe')
        if candidate.is_file():
            return str(candidate)
    return str(exe)

def find_msbuild():
    # VS 2022 Build Tools can live under Program Files (x86).
    path = shutil.which('MSBuild.exe') or shutil.which('msbuild')
    if path:
        return path
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if not vswhere.exists():
        return None
    try:
        run = subprocess.run([str(vswhere), '-latest', '-products', '*',
            '-version', '[17.0,18.0)', '-requires', 'Microsoft.Component.MSBuild',
            '-property', 'installationPath'], capture_output=True, text=True,
            timeout=15, check=True)
        install = run.stdout.strip()
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None
    if not install:
        return None
    for relative in ['MSBuild/Current/Bin/amd64/MSBuild.exe',
                     'MSBuild/Current/Bin/MSBuild.exe']:
        candidate = Path(install) / relative
        if candidate.is_file():
            return str(candidate)
    return None

class BuildApp:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title('Pujjo SOF - source development only')
        self.root.geometry('880x570')
        self.root.minsize(670, 380)
        self.queue = queue.Queue()
        self.busy = False
        top = tk.Frame(self.root)
        top.pack(fill='x', padx=12, pady=10)
        tk.Label(top, text='NOT INSTALLABLE - Audio hardware control and speaker playback unfinished',
                 fg='#b22222', font=('Segoe UI', 11, 'bold')).pack(anchor='w')
        tk.Label(top, text='Nothing here installs, signs, loads or activates a driver.').pack(anchor='w')
        buttons = tk.Frame(self.root)
        buttons.pack(fill='x', padx=12, pady=(0, 8))
        self.test = tk.Button(buttons, text='Run software checks', command=self.run_tests)
        self.test.pack(side='left', padx=(0, 8))
        self.build = tk.Button(buttons, text='Compile all projects (x64)', command=self.run_build)
        self.build.pack(side='left', padx=(0, 8))
        tk.Button(buttons, text='Open build folder', command=self.open_folder).pack(side='left')
        self.log = scrolledtext.ScrolledText(self.root, wrap='word', font=('Consolas', 10))
        self.log.pack(expand=True, fill='both', padx=12, pady=(0, 12))
        self.put('Ready. You can run software checks without the Windows Driver Kit.\n')
        self.put('Compilation requires Visual Studio 2022 C++ Build Tools + WDK/SDK 26100.\n')
        self.root.after(100, self.poll)

    def put(self, text):
        self.log.insert('end', text)
        self.log.see('end')

    def poll(self):
        try:
            while True:
                kind, data = self.queue.get_nowait()
                if kind == 'log':
                    self.put(data)
                elif kind == 'done':
                    self.busy = False
                    self.test.config(state='normal')
                    self.build.config(state='normal')
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def begin(self, task):
        if self.busy:
            return
        self.busy = True
        self.test.config(state='disabled')
        self.build.config(state='disabled')
        threading.Thread(target=self.do_task, args=(task,), daemon=True).start()

    def do_task(self, task):
        try:
            task()
        except Exception as exc:
            self.queue.put(('log', f'\nERROR: {exc}\n'))
        finally:
            self.queue.put(('done', None))

    def command(self, args):
        self.queue.put(('log', '\n> ' + ' '.join(map(str, args)) + '\n'))
        proc = subprocess.Popen(args, cwd=str(ROOT), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                errors='replace', bufsize=1)
        for line in proc.stdout:
            self.queue.put(('log', line))
        return proc.wait()

    def run_tests(self):
        def job():
            ok = True
            for script in TESTS:
                rc = self.command([python_console(), str(ROOT / script)])
                if rc != 0:
                    ok = False
            self.queue.put(('log', '\nSoftware checks: ' + ('PASS' if ok else 'FAILED') + '\n'))
        self.begin(job)

    def run_build(self):
        def job():
            if sys.platform != 'win32':
                self.queue.put(('log', 'Windows is required for MSBuild/WDK.\n'))
                return
            required = [KIT / 'Include' / '10.0.26100.0' / 'km' / 'ntddk.h',
                        KIT / 'Include' / '10.0.26100.0' / 'um' / 'Windows.h']
            if not all(p.is_file() for p in required):
                self.queue.put(('log', 'WDK/SDK 26100 headers are missing.\n'))
                return
            msbuild = find_msbuild()
            if not msbuild:
                self.queue.put(('log', 'MSBuild from Visual Studio 2022 was not found.\n'))
                return
            LOG_DIR.mkdir(exist_ok=True)
            command = [msbuild, str(ROOT / 'sof_windows_driver.sln'),
                       '/m', '/t:Build', '/p:Configuration=Release',
                       '/p:Platform=x64', '/p:TargetPlatformVersion=10.0.26100.0',
                       '/v:minimal', '/fl',
                       '/flp:LogFile=' + str(LOG_DIR / 'last-build.log') + ';Verbosity=normal']
            rc = self.command(command)
            if rc:
                self.queue.put(('log', f'\nBUILD FAILED (exit {rc}). See build-logs/last-build.log\n'))
            else:
                self.queue.put(('log', '\nCompilation returned success. NOT signed, installed, tested, or functional audio.\n'))
        self.begin(job)

    def open_folder(self):
        path = ROOT / 'bin' / 'x64' / 'Release'
        if not path.exists():
            messagebox.showinfo('Nothing compiled yet', 'No build output exists yet.')
            return
        if sys.platform == 'win32':
            os.startfile(str(path))
        else:
            messagebox.showinfo('Build folder', str(path))

    def mainloop(self):
        self.root.mainloop()

if __name__ == '__main__':
    BuildApp().mainloop()
