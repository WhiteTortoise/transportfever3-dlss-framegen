// One-click installer for the Transport Fever 3 DLSS Frame Generation mod.
//
//  - finds the game through Steam's library list (or lets the user browse),
//  - writes the mod's winhttp.dll (embedded in this exe) next to the game exe,
//  - downloads NVIDIA's Streamline release from NVIDIA's GitHub, checks its
//    SHA-256 and extracts only the DLLs the mod needs into <game>\framegen\
//    (skipped when matching copies are already there),
//  - uninstall removes exactly those files again.
//
// Command line (for scripting/testing): /install, /uninstall, /quiet, /dir "<game folder>"
// Compiled with the .NET Framework 4 csc (C# 5), see build_installer.ps1.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Management;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace Tf3FrameGen
{
    static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
            var opts = Options.Parse(args);
            if (opts.Quiet)
            {
                var log = new FileLog(Path.Combine(Path.GetTempPath(), "tf3_framegen_setup.log"));
                var core = new SetupCore(log.Write, (q) => true, (d, t) => { });
                string dir = opts.Dir ?? core.FindGameDir();
                if (dir == null) { log.Write("Transport Fever 3 not found; pass /dir"); return 3; }
                bool ok = opts.Uninstall ? core.Uninstall(dir) : core.Install(dir);
                return ok ? 0 : 1;
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SetupForm(opts));
            return 0;
        }
    }

    class Options
    {
        public bool Quiet, Uninstall, Install;
        public string Dir;

        public static Options Parse(string[] args)
        {
            var o = new Options();
            for (int i = 0; i < args.Length; i++)
            {
                string a = args[i].ToLowerInvariant();
                if (a == "/quiet") o.Quiet = true;
                else if (a == "/uninstall") o.Uninstall = true;
                else if (a == "/install") o.Install = true;
                else if (a == "/dir" && i + 1 < args.Length) o.Dir = args[++i];
            }
            return o;
        }
    }

    class FileLog
    {
        readonly string path;
        public FileLog(string path) { this.path = path; File.WriteAllText(path, ""); }
        public void Write(string line) { File.AppendAllText(path, line + Environment.NewLine); }
    }

    class SetupCore
    {
        const string AppId = "3493540";
        const string GameExe = "TransportFever3.exe";
        const string WinhttpProduct = "tf3_framegen";
        const string BackupName = "winhttp.dll.tf3fg-backup";

        readonly Action<string> log;
        readonly Func<string, bool> ask;            // yes/no question -> true = continue
        readonly Action<long, long> progress;       // bytes done, total

        public SetupCore(Action<string> log, Func<string, bool> ask, Action<long, long> progress)
        {
            this.log = log;
            this.ask = ask;
            this.progress = progress;
        }

        // --- finding the game -------------------------------------------------

        public string FindGameDir()
        {
            var libraries = new List<string>();
            string steam = null;
            foreach (var root in new[] { Registry.CurrentUser, Registry.LocalMachine })
            {
                foreach (var keyName in new[] { @"Software\Valve\Steam", @"SOFTWARE\WOW6432Node\Valve\Steam" })
                {
                    using (var k = root.OpenSubKey(keyName))
                    {
                        if (k == null) continue;
                        string p = (k.GetValue("SteamPath") ?? k.GetValue("InstallPath")) as string;
                        if (!string.IsNullOrEmpty(p) && steam == null) steam = p.Replace('/', '\\');
                    }
                }
            }
            if (steam != null)
            {
                libraries.Add(steam);
                string vdf = Path.Combine(steam, @"steamapps\libraryfolders.vdf");
                if (File.Exists(vdf))
                {
                    foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s*\"([^\"]+)\""))
                    {
                        libraries.Add(m.Groups[1].Value.Replace(@"\\", @"\"));
                    }
                }
            }
            foreach (string lib in libraries)
            {
                string acf = Path.Combine(lib, @"steamapps\appmanifest_" + AppId + ".acf");
                if (!File.Exists(acf)) continue;
                Match m = Regex.Match(File.ReadAllText(acf), "\"installdir\"\\s*\"([^\"]+)\"");
                if (!m.Success) continue;
                string dir = Path.Combine(lib, @"steamapps\common", m.Groups[1].Value);
                if (IsGameDir(dir)) return dir;
            }
            return null;
        }

        public static bool IsGameDir(string dir)
        {
            return !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, GameExe));
        }

        // --- checks -----------------------------------------------------------

        bool CheckGameClosed()
        {
            if (Process.GetProcessesByName(Path.GetFileNameWithoutExtension(GameExe)).Length == 0) return true;
            log("Transport Fever 3 is running. Close the game and try again.");
            return false;
        }

        bool CheckSystem()
        {
            var gpus = new List<string>();
            try
            {
                using (var s = new ManagementObjectSearcher("SELECT Name FROM Win32_VideoController"))
                {
                    foreach (ManagementObject o in s.Get()) gpus.Add((o["Name"] as string ?? "").Trim());
                }
            }
            catch (Exception) { }
            string rtx = gpus.Find(g => Regex.IsMatch(g, @"RTX\s*(40|50)\d\d", RegexOptions.IgnoreCase));
            if (rtx != null)
            {
                log("GPU: " + rtx + " (supports DLSS Frame Generation)");
            }
            else
            {
                log("GPU: " + (gpus.Count > 0 ? string.Join(", ", gpus) : "unknown"));
                if (!ask("DLSS Frame Generation needs an NVIDIA RTX 40 or 50 series GPU, and none was found.\n\n" +
                         "Install anyway?"))
                {
                    return false;
                }
            }
            object hags = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\GraphicsDrivers",
                                            "HwSchMode", null);
            if (hags is int && (int)hags == 1)
            {
                log("Warning: Hardware-accelerated GPU scheduling is OFF. Frame Generation needs it: " +
                    "Windows Settings > System > Display > Graphics > Change default graphics settings, then restart.");
            }
            return true;
        }

        // --- install ----------------------------------------------------------

        public bool Install(string gameDir)
        {
            try
            {
                log("Game folder: " + gameDir);
                if (!IsGameDir(gameDir)) { log("TransportFever3.exe was not found in that folder."); return false; }
                if (!CheckGameClosed() || !CheckSystem()) return false;

                string winhttp = Path.Combine(gameDir, "winhttp.dll");
                if (File.Exists(winhttp) && !IsOurs(winhttp))
                {
                    string backup = Path.Combine(gameDir, BackupName);
                    log("Another program's winhttp.dll is in the game folder; keeping it as " + BackupName);
                    if (!File.Exists(backup)) File.Copy(winhttp, backup);
                }
                using (Stream s = typeof(SetupCore).Assembly.GetManifestResourceStream("tf3fg.winhttp.dll"))
                using (FileStream f = File.Create(winhttp))
                {
                    s.CopyTo(f);
                }
                log("Installed winhttp.dll (the mod)");

                string fgDir = Path.Combine(gameDir, "framegen");
                Directory.CreateDirectory(fgDir);
                var missing = new List<Generated.RuntimeFile>();
                foreach (var file in Generated.Files)
                {
                    string p = Path.Combine(fgDir, file.Name);
                    if (!File.Exists(p) || Sha256(p) != file.Sha256) missing.Add(file);
                }
                if (missing.Count == 0)
                {
                    log("NVIDIA Streamline files already present and verified");
                }
                else if (!InstallStreamline(fgDir, missing))
                {
                    return false;
                }

                foreach (string off in new[] { "tf3_framegen.disable", "tf3_framegen.nofg" })
                {
                    string p = Path.Combine(gameDir, off);
                    if (File.Exists(p)) { File.Delete(p); log("Removed " + off + " (it was switching the mod off)"); }
                }
                log("");
                log("Done. Start Transport Fever 3 and load a map: Frame Generation turns on automatically.");
                log("Ctrl+Alt+\\ toggles it (change the key in tf3_framegen.ini in the game folder).");
                return true;
            }
            catch (UnauthorizedAccessException e)
            {
                log("No permission to write to the game folder: " + e.Message);
                log("Run this installer as administrator (right-click > Run as administrator).");
                return false;
            }
            catch (Exception e)
            {
                log("Install failed: " + e.Message);
                return false;
            }
        }

        bool InstallStreamline(string fgDir, List<Generated.RuntimeFile> needed)
        {
            string cacheDir = Path.Combine(Path.GetTempPath(), "tf3_framegen");
            Directory.CreateDirectory(cacheDir);
            string zip = Path.Combine(cacheDir, Generated.StreamlineZipName);

            // A copy of the zip next to this installer avoids the download.
            string local = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, Generated.StreamlineZipName);
            if (File.Exists(local) && Sha256(local) == Generated.StreamlineZipSha256)
            {
                zip = local;
                log("Using " + Generated.StreamlineZipName + " next to the installer");
            }
            else if (!(File.Exists(zip) && Sha256(zip) == Generated.StreamlineZipSha256))
            {
                log(string.Format("Downloading NVIDIA Streamline {0} from NVIDIA's GitHub ({1} MB)...",
                                  Generated.StreamlineVersion, Generated.StreamlineZipSize / 1048576));
                Download(Generated.StreamlineUrl, zip);
                log("Checking the download...");
                if (Sha256(zip) != Generated.StreamlineZipSha256)
                {
                    File.Delete(zip);
                    log("The download does not match NVIDIA's release (checksum mismatch). Please try again.");
                    return false;
                }
            }

            using (ZipArchive archive = ZipFile.OpenRead(zip))
            {
                foreach (var file in needed)
                {
                    ZipArchiveEntry entry = archive.GetEntry("bin/x64/" + file.Name);
                    if (entry == null) { log("Missing in the NVIDIA package: " + file.Name); return false; }
                    string target = Path.Combine(fgDir, file.Name);
                    entry.ExtractToFile(target, true);
                    if (Sha256(target) != file.Sha256) { log("Checksum mismatch: " + file.Name); return false; }
                }
            }
            log("Installed NVIDIA Streamline files into the framegen folder");
            return true;
        }

        void Download(string url, string target)
        {
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;  // TLS 1.2
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.UserAgent = "tf3_framegen-setup";
            string part = target + ".part";
            using (var response = (HttpWebResponse)request.GetResponse())
            using (Stream input = response.GetResponseStream())
            using (FileStream output = File.Create(part))
            {
                long total = response.ContentLength > 0 ? response.ContentLength : Generated.StreamlineZipSize;
                byte[] buffer = new byte[1 << 20];
                long done = 0;
                int n;
                while ((n = input.Read(buffer, 0, buffer.Length)) > 0)
                {
                    output.Write(buffer, 0, n);
                    done += n;
                    progress(done, total);
                }
            }
            if (File.Exists(target)) File.Delete(target);
            File.Move(part, target);
        }

        // --- uninstall --------------------------------------------------------

        public bool Uninstall(string gameDir)
        {
            try
            {
                log("Game folder: " + gameDir);
                if (!CheckGameClosed()) return false;
                string winhttp = Path.Combine(gameDir, "winhttp.dll");
                if (File.Exists(winhttp))
                {
                    if (IsOurs(winhttp)) { File.Delete(winhttp); log("Removed winhttp.dll (the mod)"); }
                    else log("winhttp.dll belongs to another program; left in place");
                }
                string backup = Path.Combine(gameDir, BackupName);
                if (File.Exists(backup) && !File.Exists(winhttp))
                {
                    File.Move(backup, winhttp);
                    log("Restored the other program's winhttp.dll");
                }
                string fgDir = Path.Combine(gameDir, "framegen");
                if (Directory.Exists(fgDir))
                {
                    foreach (var file in Generated.Files) DeleteIfExists(Path.Combine(fgDir, file.Name));
                    foreach (string extra in new[] { "sl.log", "nvngx_dlss.license.txt", "reflex.license.txt" })
                    {
                        DeleteIfExists(Path.Combine(fgDir, extra));
                    }
                    if (Directory.GetFileSystemEntries(fgDir).Length == 0) Directory.Delete(fgDir);
                    log("Removed the framegen folder");
                }
                foreach (string f in new[] { "tf3_framegen.ini", "tf3_framegen.log", "tf3_framegen.disable", "tf3_framegen.nofg" })
                {
                    DeleteIfExists(Path.Combine(gameDir, f));
                }
                log("");
                log("The mod is uninstalled. The game runs exactly as before.");
                return true;
            }
            catch (Exception e)
            {
                log("Uninstall failed: " + e.Message);
                return false;
            }
        }

        // --- helpers ----------------------------------------------------------

        public static bool IsInstalled(string gameDir)
        {
            string p = Path.Combine(gameDir, "winhttp.dll");
            return File.Exists(p) && IsOurs(p);
        }

        static bool IsOurs(string path)
        {
            try
            {
                if (FileVersionInfo.GetVersionInfo(path).ProductName == WinhttpProduct) return true;
                // Builds from before the version resource: recognise our log file name in the binary.
                byte[] data = File.ReadAllBytes(path);
                byte[] marker = Encoding.Unicode.GetBytes("tf3_framegen.log");
                for (int i = 0; i + marker.Length <= data.Length; i++)
                {
                    int k = 0;
                    while (k < marker.Length && data[i + k] == marker[k]) k++;
                    if (k == marker.Length) return true;
                }
                return false;
            }
            catch (Exception) { return false; }
        }

        static void DeleteIfExists(string p)
        {
            if (File.Exists(p)) File.Delete(p);
        }

        static string Sha256(string path)
        {
            using (var sha = SHA256.Create())
            using (FileStream f = File.OpenRead(path))
            {
                return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
            }
        }
    }

    class SetupForm : Form
    {
        readonly TextBox dirBox = new TextBox();
        readonly TextBox logBox = new TextBox();
        readonly ProgressBar bar = new ProgressBar();
        readonly Button installButton = new Button();
        readonly Button uninstallButton = new Button();
        readonly Button browseButton = new Button();
        readonly Button openButton = new Button();
        readonly Label stateLabel = new Label();
        readonly SetupCore core;

        public SetupForm(Options opts)
        {
            Text = "Transport Fever 3 - DLSS Frame Generation";
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(620, 440);
            MinimumSize = new Size(560, 400);
            StartPosition = FormStartPosition.CenterScreen;

            var title = new Label
            {
                Text = "DLSS Frame Generation for Transport Fever 3",
                Font = new Font("Segoe UI", 13f, FontStyle.Bold),
                AutoSize = true,
                Location = new Point(16, 14)
            };
            var info = new Label
            {
                Text = "Needs an NVIDIA RTX 40 or 50 series GPU. NVIDIA's Streamline files are downloaded from NVIDIA's\n" +
                       "GitHub during install (about 276 MB, only the needed DLLs are kept).",
                AutoSize = true,
                Location = new Point(18, 46)
            };
            var dirLabel = new Label { Text = "Game folder:", AutoSize = true, Location = new Point(18, 92) };
            dirBox.Location = new Point(100, 88);
            dirBox.Width = 400;
            dirBox.Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right;
            dirBox.TextChanged += (s, e) => UpdateState();
            browseButton.Text = "Browse...";
            browseButton.Location = new Point(510, 86);
            browseButton.Width = 94;
            browseButton.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            browseButton.Click += (s, e) => Browse();
            stateLabel.AutoSize = true;
            stateLabel.Location = new Point(98, 116);

            logBox.Multiline = true;
            logBox.ReadOnly = true;
            logBox.ScrollBars = ScrollBars.Vertical;
            logBox.Location = new Point(18, 142);
            logBox.Size = new Size(586, 220);
            logBox.Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right | AnchorStyles.Bottom;
            logBox.BackColor = SystemColors.Window;

            bar.Location = new Point(18, 370);
            bar.Size = new Size(586, 14);
            bar.Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Bottom;

            installButton.Text = "Install";
            installButton.Size = new Size(110, 32);
            installButton.Location = new Point(18, 396);
            installButton.Anchor = AnchorStyles.Left | AnchorStyles.Bottom;
            installButton.Click += (s, e) => Run(false);
            uninstallButton.Text = "Uninstall";
            uninstallButton.Size = new Size(110, 32);
            uninstallButton.Location = new Point(136, 396);
            uninstallButton.Anchor = AnchorStyles.Left | AnchorStyles.Bottom;
            uninstallButton.Click += (s, e) => Run(true);
            openButton.Text = "Open game folder";
            openButton.Size = new Size(140, 32);
            openButton.Location = new Point(464, 396);
            openButton.Anchor = AnchorStyles.Right | AnchorStyles.Bottom;
            openButton.Click += (s, e) => { if (SetupCore.IsGameDir(dirBox.Text)) Process.Start(dirBox.Text); };

            Controls.AddRange(new Control[] { title, info, dirLabel, dirBox, browseButton, stateLabel, logBox, bar,
                                              installButton, uninstallButton, openButton });

            core = new SetupCore(Log, Ask, Progress);
            AcceptButton = installButton;
            string dir = opts.Dir ?? core.FindGameDir();
            if (dir != null) dirBox.Text = dir;
            else Log("Transport Fever 3 was not found automatically. Use Browse to pick its folder.");
            UpdateState();
        }

        void UpdateState()
        {
            string dir = dirBox.Text;
            if (!SetupCore.IsGameDir(dir))
            {
                stateLabel.Text = "TransportFever3.exe not found in this folder";
                stateLabel.ForeColor = Color.Firebrick;
                installButton.Enabled = uninstallButton.Enabled = false;
                return;
            }
            bool installed = SetupCore.IsInstalled(dir);
            stateLabel.Text = installed ? "The mod is installed (Install again to repair or update)" : "The mod is not installed";
            stateLabel.ForeColor = installed ? Color.SeaGreen : SystemColors.ControlText;
            installButton.Enabled = true;
            installButton.Text = installed ? "Reinstall" : "Install";
            uninstallButton.Enabled = installed;
        }

        void Browse()
        {
            using (var dlg = new FolderBrowserDialog())
            {
                dlg.Description = "Select the Transport Fever 3 folder (the one containing TransportFever3.exe)";
                if (SetupCore.IsGameDir(dirBox.Text)) dlg.SelectedPath = dirBox.Text;
                if (dlg.ShowDialog(this) == DialogResult.OK) dirBox.Text = dlg.SelectedPath;
            }
        }

        void Run(bool uninstall)
        {
            string dir = dirBox.Text;
            SetBusy(true);
            logBox.Clear();
            bar.Value = 0;
            var t = new Thread(() =>
            {
                bool ok = uninstall ? core.Uninstall(dir) : core.Install(dir);
                BeginInvoke((Action)(() =>
                {
                    SetBusy(false);
                    bar.Value = ok ? bar.Maximum : 0;
                    UpdateState();
                }));
            });
            t.IsBackground = true;
            t.Start();
        }

        void SetBusy(bool busy)
        {
            installButton.Enabled = uninstallButton.Enabled = browseButton.Enabled = dirBox.Enabled = !busy;
            UseWaitCursor = busy;
        }

        void Log(string line)
        {
            if (InvokeRequired) { BeginInvoke((Action)(() => Log(line))); return; }
            logBox.AppendText(line + Environment.NewLine);
        }

        bool Ask(string question)
        {
            if (InvokeRequired) return (bool)Invoke((Func<bool>)(() => Ask(question)));
            return MessageBox.Show(this, question, Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) ==
                   DialogResult.Yes;
        }

        void Progress(long done, long total)
        {
            if (InvokeRequired) { BeginInvoke((Action)(() => Progress(done, total))); return; }
            bar.Maximum = 1000;
            bar.Value = (int)Math.Min(1000, done * 1000 / Math.Max(1, total));
        }
    }
}
