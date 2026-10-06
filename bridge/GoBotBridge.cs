// A way in to the other helper's compiled code.
//
// Everything else in this project reads that helper: its pictures, its
// resource names, its feature list, its login trace. This calls it. GoBot is
// C++ and the helper is a .NET 6 assembly, so something has to stand between
// them, and this is it: a small library the C++ side loads through the .NET
// hosting API, which then reaches into TungHaVGOBot.dll by reflection.
//
// Entry points take (IntPtr buffer, int capacity) and return the number of
// bytes they wanted to write. That is the shape .NET's hosting API calls a
// component entry point, and using it means none of this needs the
// UnmanagedCallersOnly attribute, unsafe code, or a generated apphost -- which
// matters, because the only C# compiler on this machine is the one inside
// Visual Studio's build tools and there is no .NET SDK to lean on.
//
// What is realistic to call is an open question, and answering it is what
// Probe is for. The helper's features are mostly partial pieces of its
// MainWindow -- a WPF window wired to its own settings objects and to its
// subscription check -- so "call its fishing loop" is not a thing that can be
// done from outside. Its standalone pieces are another matter, and Probe says
// which those are rather than guessing.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace GoBot
{
    public static class Bridge
    {
        const string HelperDir = @"C:\Program Files\TH_VGO_Bot_Helper";

        static Assembly _helper;
        static bool _resolverHooked;

        /// Load the helper, and everything it leans on.
        ///
        /// Its dependencies -- OpenCvSharp, MahApps, SQLite, Tesseract -- sit
        /// beside it in its own folder rather than anywhere .NET would look, so
        /// without this hook the first reflection call that touches one throws
        /// a file-not-found for an assembly that is plainly right there.
        static Assembly Helper()
        {
            if (_helper != null) return _helper;

            if (!_resolverHooked)
            {
                _resolverHooked = true;
                AppDomain.CurrentDomain.AssemblyResolve += (sender, args) =>
                {
                    var wanted = new AssemblyName(args.Name).Name + ".dll";
                    var path = Path.Combine(HelperDir, wanted);
                    return File.Exists(path) ? Assembly.LoadFrom(path) : null;
                };
            }

            _helper = Assembly.LoadFrom(Path.Combine(HelperDir, "TungHaVGOBot.dll"));
            return _helper;
        }

        static int Write(string text, IntPtr buffer, int capacity)
        {
            var bytes = Encoding.UTF8.GetBytes(text);
            if (buffer != IntPtr.Zero && capacity > 1)
            {
                int n = Math.Min(bytes.Length, capacity - 1);
                Marshal.Copy(bytes, 0, buffer, n);
                Marshal.WriteByte(buffer, n, 0);
            }
            return bytes.Length;
        }

        /// Does the assembly load at all, and what is it?
        public static int Hello(IntPtr buffer, int capacity)
        {
            try
            {
                var asm = Helper();
                var name = asm.GetName();
                var types = asm.GetTypes().Length;
                return Write(
                    $"{name.Name} {name.Version}\n{types} types\n{asm.Location}\n",
                    buffer, capacity);
            }
            catch (Exception e)
            {
                return Write("ERROR " + Flatten(e), buffer, capacity);
            }
        }

        /// What is in there that could actually be called from outside.
        ///
        /// Instance methods on a WPF window are listed too, and listed as such,
        /// because knowing that the fishing loop is a method on MainWindow is
        /// precisely the finding that says it cannot be called.
        public static int Probe(IntPtr buffer, int capacity)
        {
            try
            {
                var asm = Helper();
                var report = new StringBuilder();

                Type[] types;
                try { types = asm.GetTypes(); }
                catch (ReflectionTypeLoadException partial)
                {
                    types = partial.Types.Where(t => t != null).ToArray();
                    report.Append("note: ").Append(partial.LoaderExceptions.Length)
                          .Append(" types failed to load; listing the rest\n\n");
                }

                // Narrowed by a request when one is given, so a probe can ask
                // about one area instead of printing everything every time.
                var request = buffer == IntPtr.Zero ? "" : (Marshal.PtrToStringUTF8(buffer) ?? "").Trim();
                var wanted = request.Length > 0
                    ? request.Split(',', StringSplitOptions.RemoveEmptyEntries)
                             .Select(s => s.Trim()).Where(s => s.Length > 0).ToArray()
                    : new[] {
                        "TemplateMatch", "ScanHPMP", "HpMp", "SharedFrame", "Automation",
                        "Fishing", "Mining", "Party", "Cleanup", "VGOAction", "Ocr",
                      };

                foreach (var type in types.OrderBy(t => t.FullName, StringComparer.Ordinal))
                {
                    var full = type.FullName ?? "";
                    if (!wanted.Any(w => full.IndexOf(w, StringComparison.OrdinalIgnoreCase) >= 0))
                        continue;

                    report.Append(type.IsPublic ? "public  " : "internal ")
                          .Append(type.IsAbstract && type.IsSealed ? "static " : "")
                          .Append(full).Append('\n');

                    const BindingFlags all = BindingFlags.Public | BindingFlags.NonPublic |
                                             BindingFlags.Static | BindingFlags.Instance |
                                             BindingFlags.DeclaredOnly;
                    foreach (var method in type.GetMethods(all).OrderBy(m => m.Name, StringComparer.Ordinal))
                    {
                        if (method.IsSpecialName) continue;
                        report.Append("    ")
                              .Append(method.IsStatic ? "static " : "       ")
                              .Append(method.Name).Append('(')
                              .Append(string.Join(", ", method.GetParameters()
                                      .Select(p => p.ParameterType.Name)))
                              .Append(") -> ").Append(method.ReturnType.Name).Append('\n');
                    }
                    report.Append('\n');
                }

                if (report.Length == 0) report.Append("nothing matched the names looked for\n");
                return Write(report.ToString(), buffer, capacity);
            }
            catch (Exception e)
            {
                return Write("ERROR " + Flatten(e), buffer, capacity);
            }
        }

        /// Click, using the helper's own input code rather than ours.
        ///
        /// The buffer arrives carrying "x,y" in screen coordinates and leaves
        /// carrying what happened. Their two synchronous entry points are used:
        /// MoveMouseAbsoluteCore to place the pointer and CustomMouseClick to
        /// press it. Everything else in their input class returns a Task, which
        /// is awkward to await from a plain function pointer and unnecessary
        /// when these two will do.
        ///
        /// Worth preferring to ours for one specific reason: it is the code
        /// that is known to work on this game. Ours sends the same kind of
        /// input and should be equivalent, and "should be equivalent" is what
        /// an evening gets spent on when a click quietly fails to land.
        public static int Click(IntPtr buffer, int capacity)
        {
            string request = "";
            try
            {
                if (buffer != IntPtr.Zero) request = Marshal.PtrToStringUTF8(buffer) ?? "";
                var bits = request.Split(',');
                if (bits.Length < 2 ||
                    !int.TryParse(bits[0].Trim(), out int x) ||
                    !int.TryParse(bits[1].Trim(), out int y))
                    return Write("ERROR expected \"x,y\", got \"" + request + "\"",
                                 buffer, capacity);

                var asm = Helper();
                var input = asm.GetType("TungHaVGOBot.AutomationInput");
                if (input == null)
                    return Write("ERROR no TungHaVGOBot.AutomationInput in that assembly",
                                 buffer, capacity);

                const BindingFlags statics = BindingFlags.Public | BindingFlags.NonPublic |
                                             BindingFlags.Static;

                var move = input.GetMethod("MoveMouseAbsoluteCore", statics);
                var click = input.GetMethod("CustomMouseClick", statics);
                if (move == null || click == null)
                    return Write("ERROR that assembly has no MoveMouseAbsoluteCore/"
                                 + "CustomMouseClick to call", buffer, capacity);

                var where = move.Invoke(null, new object[] { x, y });

                // Their second argument is a flag whose meaning is not in the
                // metadata; false is the conservative reading of it.
                click.Invoke(null, new object[] { "left", false });

                return Write($"clicked {x},{y} through TungHaVGOBot.AutomationInput"
                             + (where != null ? " (it reported " + where + ")" : ""),
                             buffer, capacity);
            }
            catch (Exception e)
            {
                return Write("ERROR " + Flatten(e), buffer, capacity);
            }
        }

        const BindingFlags Any = BindingFlags.Public | BindingFlags.NonPublic |
                                 BindingFlags.Instance | BindingFlags.Static;

        const BindingFlags Statics =
            BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;

        /// Everything about the party, read by the helper's own detector.
        ///
        /// Their party code is the one part of the helper that is entirely
        /// callable from outside: PartyStatusDetector, PartyStatusRules and
        /// PartyConfigureDetector are public static classes taking plain
        /// arguments, where their fishing and mining are methods on a WPF
        /// window and cannot be reached at all.
        ///
        /// The request is the game window's handle in decimal. What comes back
        /// is what their detector made of the screen, what their own rules say
        /// may be done about it, and -- because their configure detector knows
        /// where the dropdowns are -- the points to click to set sharing.
        public static int PartyStatus(IntPtr buffer, int capacity)
        {
            try
            {
                var request = buffer == IntPtr.Zero ? "" : (Marshal.PtrToStringUTF8(buffer) ?? "");
                if (!long.TryParse(request.Trim(), out long raw))
                    return Write("ERROR expected the game window handle, got \"" +
                                 request + "\"", buffer, capacity);
                var hwnd = new IntPtr(raw);

                var asm = Helper();
                var detector = asm.GetType("TungHaVGOBot.Core.PartyStatusDetector");
                var rules = asm.GetType("TungHaVGOBot.Core.PartyStatusRules");
                if (detector == null || rules == null)
                    return Write("ERROR that assembly has no PartyStatusDetector", buffer, capacity);

                var detect = detector.GetMethod("Detect", Statics);
                if (detect == null)
                    return Write("ERROR PartyStatusDetector has no Detect", buffer, capacity);

                var result = detect.Invoke(null, new object[] { hwnd });
                var report = new StringBuilder();
                report.Append("detector said: ").Append(result ?? "(null)").Append('\n');

                // Pull the runtime status out of whatever shape the result is,
                // by looking for the member whose type the rules want.
                object status = null;
                if (result != null)
                {
                    foreach (var p in result.GetType().GetProperties())
                    {
                        var v = p.GetValue(result);
                        report.Append("  ").Append(p.Name).Append(" = ").Append(v ?? "null").Append('\n');
                        if (p.PropertyType.Name == "PartyRuntimeStatus") status = v;
                    }
                    foreach (var f in result.GetType().GetFields())
                    {
                        var v = f.GetValue(result);
                        report.Append("  ").Append(f.Name).Append(" = ").Append(v ?? "null").Append('\n');
                        if (f.FieldType.Name == "PartyRuntimeStatus") status = v;
                    }
                }

                if (status != null)
                {
                    report.Append("\ntheir rules say:\n");
                    foreach (var name in new[] { "CanAutoAccept", "CanAutoInvite",
                                                 "CanTransferPartyKey", "FormatOverview" })
                    {
                        var m = rules.GetMethod(name, Statics);
                        if (m == null) continue;
                        report.Append("  ").Append(name).Append(" -> ")
                              .Append(m.Invoke(null, new[] { status })).Append('\n');
                    }
                }
                else
                {
                    report.Append("\n(no PartyRuntimeStatus in that result, so the "
                                + "rules were not asked)\n");
                }

                report.Append('\n').Append(ConfigurePoints(asm, ClientWidth(hwnd)));
                return Write(report.ToString(), buffer, capacity);
            }
            catch (Exception e)
            {
                return Write("ERROR " + Flatten(e), buffer, capacity);
            }
        }

        /// Who has asked to join, by chatting the keyword.
        ///
        /// Their PtInviteFromChat does the whole of this: it captures the chat
        /// panel, reads it with their own bitmap-font matcher, finds the
        /// keyword, works out which name said it, and confirms the name against
        /// the player list. `DetectPtNamesOnce` takes the game's window and
        /// answers with the names.
        ///
        /// Unlike their party detector this is an instance method, so the
        /// object has to be made first -- and whether that can be done from
        /// outside is the open question this answers. If it cannot, the reply
        /// says what the constructors want, which is the next thing anyone
        /// would need to know.
        ///
        /// The request is "<hwnd>" or "<hwnd>|<keyword>".
        public static int ChatInvites(IntPtr buffer, int capacity)
        {
            try
            {
                var request = buffer == IntPtr.Zero ? "" : (Marshal.PtrToStringUTF8(buffer) ?? "");
                var bits = request.Split('|');
                if (bits.Length < 1 || !long.TryParse(bits[0].Trim(), out long raw))
                    return Write("error=expected \"<hwnd>\" or \"<hwnd>|<keyword>\"",
                                 buffer, capacity);
                var hwnd = new IntPtr(raw);
                var keyword = bits.Length > 1 ? bits[1].Trim() : "";

                var asm = Helper();
                var type = asm.GetType("TungHaVGOBot.PtInviteFromChat");
                if (type == null) return Write("error=no PtInviteFromChat", buffer, capacity);

                // It takes a Config -- an internal nested type. Make one, fill
                // in the keyword, and describe the rest in the reply, because
                // what that object wants is the only thing standing between
                // here and a working invite.
                var configType = type.GetNestedType("Config", Any);
                if (configType == null)
                    return Write("error=PtInviteFromChat wants a Config and there "
                               + "is no nested Config type to make", buffer, capacity);

                // Their Config is a record with a positional constructor, so it
                // is built by filling in each parameter rather than by default
                // construction. Anything not obviously ours gets the type's own
                // default, which is enough for the detector to run; the keyword
                // is the one that has to be right.
                object config = null;
                string how = "";
                foreach (var ctor in configType.GetConstructors(Any)
                                               .OrderByDescending(c => c.GetParameters().Length))
                {
                    var wants = ctor.GetParameters();
                    var args = new object[wants.Length];
                    var described = new List<string>();
                    for (int i = 0; i < wants.Length; ++i)
                    {
                        var p = wants[i];
                        described.Add(p.ParameterType.Name + " " + p.Name);

                        if (p.ParameterType == typeof(string) &&
                            p.Name != null &&
                            p.Name.IndexOf("keyword", StringComparison.OrdinalIgnoreCase) >= 0 &&
                            keyword.Length > 0)
                            args[i] = keyword;
                        else if (p.HasDefaultValue)
                            args[i] = p.DefaultValue;
                        else if (p.ParameterType == typeof(string))
                            args[i] = "";
                        else if (p.ParameterType.IsValueType)
                            args[i] = Activator.CreateInstance(p.ParameterType);
                        else
                            args[i] = null;
                    }

                    how = "config_ctor=" + string.Join(", ", described) + "\n";
                    try { config = ctor.Invoke(args); break; }
                    catch (Exception e)
                    {
                        how += "config_ctor_failed=" + Flatten(e).Replace('\n', ' ') + "\n";
                    }
                }

                if (config == null)
                    return Write("error=cannot make its Config\n" + how, buffer, capacity);

                var shape = new StringBuilder(how);
                foreach (var p in configType.GetProperties(Any))
                {
                    object had = null;
                    try { had = p.GetValue(config); } catch { }
                    shape.Append("config.").Append(p.Name).Append(':')
                         .Append(p.PropertyType.Name).Append('=').Append(Flat(had)).Append('\n');
                    if (keyword.Length > 0 && p.CanWrite && p.PropertyType == typeof(string) &&
                        p.Name.IndexOf("keyword", StringComparison.OrdinalIgnoreCase) >= 0)
                        try { p.SetValue(config, keyword); } catch { }
                }
                foreach (var f in configType.GetFields(Any))
                {
                    object had = null;
                    try { had = f.GetValue(config); } catch { }
                    shape.Append("config.").Append(f.Name).Append(':')
                         .Append(f.FieldType.Name).Append('=').Append(Flat(had)).Append('\n');
                    if (keyword.Length > 0 && f.FieldType == typeof(string) &&
                        f.Name.IndexOf("keyword", StringComparison.OrdinalIgnoreCase) >= 0)
                        try { f.SetValue(config, keyword); } catch { }
                }

                object instance;
                try
                {
                    instance = Activator.CreateInstance(type, BindingFlags.Instance |
                                                        BindingFlags.Public | BindingFlags.NonPublic,
                                                        null, new[] { config }, null);
                }
                catch (Exception e)
                {
                    return Write("error=cannot construct it: " +
                                 Flatten(e).Replace('\n', ' ') + "\n" + shape,
                                 buffer, capacity);
                }

                var detect = type.GetMethod("DetectPtNamesOnce",
                                            BindingFlags.Public | BindingFlags.NonPublic |
                                            BindingFlags.Instance);
                if (detect == null)
                    return Write("error=no DetectPtNamesOnce on it", buffer, capacity);

                var found = detect.Invoke(instance, new object[] { hwnd });
                var lines = new StringBuilder();
                int n = 0;
                if (found is System.Collections.IEnumerable list)
                    foreach (var item in list)
                    {
                        lines.Append("name=").Append(item).Append('\n');
                        ++n;
                    }

                lines.Insert(0, "count=" + n + "\n");
                lines.Append(shape);
                return Write(lines.ToString(), buffer, capacity);
            }
            catch (Exception e)
            {
                return Write("error=" + Flatten(e).Replace('\n', ' '), buffer, capacity);
            }
        }

        /// The same thing, in a form a program can read.
        ///
        /// PartyStatus is for a person: it prints whatever shape their result
        /// happens to be. This is for the bot, so it is flat `key=value` lines
        /// and the keys do not change when their field names do.
        public static int PartyState(IntPtr buffer, int capacity)
        {
            try
            {
                var request = buffer == IntPtr.Zero ? "" : (Marshal.PtrToStringUTF8(buffer) ?? "");
                if (!long.TryParse(request.Trim(), out long raw))
                    return Write("error=expected the game window handle", buffer, capacity);
                var hwnd = new IntPtr(raw);

                var asm = Helper();
                var detector = asm.GetType("TungHaVGOBot.Core.PartyStatusDetector");
                var rules = asm.GetType("TungHaVGOBot.Core.PartyStatusRules");
                var detect = detector?.GetMethod("Detect", Statics);
                if (detect == null) return Write("error=no PartyStatusDetector", buffer, capacity);

                var result = detect.Invoke(null, new object[] { hwnd });
                if (result == null) return Write("error=the detector returned nothing", buffer, capacity);

                var lines = new StringBuilder();
                object status = null;
                foreach (var p in result.GetType().GetProperties())
                {
                    var v = p.GetValue(result);
                    if (p.PropertyType.Name == "PartyRuntimeStatus") status = v;
                    lines.Append(Key(p.Name)).Append('=').Append(Flat(v)).Append('\n');
                }

                if (status != null && rules != null)
                    foreach (var name in new[] { "CanAutoAccept", "CanAutoInvite", "CanTransferPartyKey" })
                    {
                        var m = rules.GetMethod(name, Statics);
                        if (m != null)
                            lines.Append(Key(name)).Append('=').Append(Flat(m.Invoke(null, new[] { status }))).Append('\n');
                    }

                // The click points, so the caller never has to measure them.
                var configure = asm.GetType("TungHaVGOBot.Core.PartyConfigureDetector");
                if (configure != null)
                {
                    int width = ClientWidth(hwnd);
                    lines.Append("client_width=").Append(width).Append('\n');
                    Point(lines, "exp_dropdown", configure.GetMethod("ExpDropdownClickClient", Statics), width);
                    Point(lines, "loot_dropdown", configure.GetMethod("LootDropdownClickClient", Statics), width);

                    foreach (var pair in new[] {
                        ("ExpOptionClickClient", "PartyExpShareMode", "exp"),
                        ("LootOptionClickClient", "PartyLootMode", "loot") })
                    {
                        var mode = asm.GetType("TungHaVGOBot.Core." + pair.Item2);
                        var click = configure.GetMethod(pair.Item1, Statics);
                        if (mode == null || !mode.IsEnum || click == null) continue;
                        foreach (var value in Enum.GetValues(mode))
                        {
                            var at = click.Invoke(null, new object[] { width, value });
                            lines.Append(pair.Item3).Append('_').Append(Key(value.ToString()))
                                 .Append('=').Append(XY(at)).Append('\n');
                        }
                    }
                }
                return Write(lines.ToString(), buffer, capacity);
            }
            catch (Exception e)
            {
                return Write("error=" + Flatten(e).Replace('\n', ' '), buffer, capacity);
            }
        }

        static string Key(string name)
        {
            var sb = new StringBuilder();
            for (int i = 0; i < name.Length; ++i)
            {
                if (char.IsUpper(name[i]) && i > 0) sb.Append('_');
                sb.Append(char.ToLowerInvariant(name[i]));
            }
            return sb.ToString();
        }

        static string Flat(object v) =>
            v == null ? "" : v is bool b ? (b ? "1" : "0") : v.ToString();

        static string XY(object position)
        {
            if (position == null) return "";
            var type = position.GetType();
            var x = type.GetProperty("XPos") ?? type.GetProperty("X");
            var y = type.GetProperty("YPos") ?? type.GetProperty("Y");
            if (x == null || y == null) return "";
            return x.GetValue(position) + "," + y.GetValue(position);
        }

        static void Point(StringBuilder into, string key, MethodInfo m, int width)
        {
            if (m == null) return;
            into.Append(key).Append('=').Append(XY(m.Invoke(null, new object[] { width }))).Append('\n');
        }

        /// Where their configure detector says the sharing dropdowns are.
        ///
        /// Worth having on its own: these are the click points for setting
        /// experience and loot sharing, worked out by their code rather than
        /// guessed from a screenshot -- which is exactly the kind of number
        /// this project has otherwise had to measure by hand and get wrong.
        [DllImport("user32.dll")]
        static extern bool GetClientRect(IntPtr hWnd, out RECT rect);

        [StructLayout(LayoutKind.Sequential)]
        struct RECT { public int Left, Top, Right, Bottom; }

        /// The int these detectors take is the game's client width, not an
        /// index. Passing zero gives a one-pixel-wide panel and click points
        /// all at the left edge, which looks like an answer and is not one.
        static int ClientWidth(IntPtr hwnd)
        {
            return GetClientRect(hwnd, out var r) && r.Right > r.Left ? r.Right - r.Left : 1024;
        }

        /// Their Position and Rectangle types print as their own names, which
        /// is no use to a caller that wants the numbers.
        static string Unpack(object value)
        {
            if (value == null) return "null";
            var type = value.GetType();
            if (type.IsPrimitive || value is string) return value.ToString();

            var bits = new List<string>();
            foreach (var p in type.GetProperties())
                if (p.CanRead && p.GetIndexParameters().Length == 0 &&
                    (p.PropertyType.IsPrimitive || p.PropertyType == typeof(string)))
                    bits.Add(p.Name + "=" + p.GetValue(value));
            foreach (var f in type.GetFields())
                if (f.IsPublic && (f.FieldType.IsPrimitive || f.FieldType == typeof(string)))
                    bits.Add(f.Name + "=" + f.GetValue(value));

            return bits.Count > 0 ? "{" + string.Join(", ", bits) + "}" : value.ToString();
        }

        static string ConfigurePoints(Assembly asm, int clientWidth)
        {
            var configure = asm.GetType("TungHaVGOBot.Core.PartyConfigureDetector");
            if (configure == null) return "no PartyConfigureDetector in that assembly\n";

            var report = new StringBuilder("their configure detector (client width ")
                             .Append(clientWidth).Append("):\n");
            try
            {
                var roi = configure.GetMethod("BuildConfigureRoi", Statics);
                if (roi != null)
                    report.Append("  panel roi = ")
                          .Append(Unpack(roi.Invoke(null, new object[] { clientWidth }))).Append('\n');

                foreach (var name in new[] { "ExpDropdownClickClient", "LootDropdownClickClient" })
                {
                    var m = configure.GetMethod(name, Statics);
                    if (m != null)
                        report.Append("  ").Append(name).Append(" = ")
                              .Append(Unpack(m.Invoke(null, new object[] { clientWidth }))).Append('\n');
                }

                // Every option of each mode, with where to click it.
                foreach (var pair in new[] {
                    ("ExpOptionClickClient", "PartyExpShareMode", "GetExpShareTemplate"),
                    ("LootOptionClickClient", "PartyLootMode", "GetLootTemplate") })
                {
                    var mode = asm.GetType("TungHaVGOBot.Core." + pair.Item2);
                    var click = configure.GetMethod(pair.Item1, Statics);
                    var tpl = configure.GetMethod(pair.Item3, Statics);
                    if (mode == null || !mode.IsEnum || click == null) continue;

                    foreach (var value in Enum.GetValues(mode))
                    {
                        report.Append("  ").Append(pair.Item2).Append('.').Append(value)
                              .Append(" -> click ")
                              .Append(Unpack(click.Invoke(null, new object[] { clientWidth, value })));
                        if (tpl != null)
                            report.Append(", picture ").Append(tpl.Invoke(null, new[] { value }));
                        report.Append('\n');
                    }
                }
            }
            catch (Exception e)
            {
                report.Append("  (stopped: ").Append(Flatten(e)).Append(")\n");
            }
            return report.ToString();
        }

        static string Flatten(Exception e)
        {
            var parts = new List<string>();
            for (var x = e; x != null; x = x.InnerException)
                parts.Add(x.GetType().Name + ": " + x.Message);
            return string.Join("\n  caused by ", parts);
        }
    }
}
