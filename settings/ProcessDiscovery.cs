using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace ConfiguratorThemeSettings;

internal readonly record struct ProcessIdentity(int ProcessId, ulong CreationFileTime);

internal sealed record DetectedProcess(ProcessIdentity Identity, string? Mode, string? Platform,
                                       string? BaseId, string? DiscoveryStatus = null);

internal static class OneCArguments
{
    public static (string? Mode, string? BaseId) Parse(string commandLine, bool thinClient)
    {
        if (string.IsNullOrWhiteSpace(commandLine))
            return (thinClient ? "Enterprise" : null, null);
        var argv = CommandLineToArgvW(commandLine, out var count);
        if (argv == IntPtr.Zero)
            return (thinClient ? "Enterprise" : null, null);
        try
        {
            var mode = count > 1 ? ReadArg(argv, 1).ToUpperInvariant() switch
            {
                "DESIGNER" when !thinClient => "Designer",
                "ENTERPRISE" => "Enterprise",
                _ => null
            } : null;
            var firstOption = mode is null ? 1 : 2;
            mode ??= thinClient ? "Enterprise" : null;
            string? baseId = null;
            for (var i = firstOption; i < count; i++)
            {
                var argument = ReadArg(argv, i);
                var option = argument.ToUpperInvariant();
                if (option is "/N" or "/P" or "/WSN" or "/WSP" or "/WSPUSER" or "/WSPPWD")
                {
                    i++;
                    continue;
                }
                if (option is "/SLEV" or "/SLEV0" or "/SLEV1" or "/SLEV2")
                    continue;
                if (option.StartsWith("/IBNAME", StringComparison.Ordinal) ||
                    option.StartsWith("/IBCONNECTIONSTRING", StringComparison.Ordinal) ||
                    option.StartsWith("/@", StringComparison.Ordinal) || option == "/WS" ||
                    option.StartsWith("/WSHTTP:", StringComparison.Ordinal) ||
                    option.StartsWith("/WSHTTPS:", StringComparison.Ordinal))
                    return (mode, null);
                var prefix = argument.StartsWith("/S", StringComparison.OrdinalIgnoreCase) ? "server:" :
                             argument.StartsWith("/F", StringComparison.OrdinalIgnoreCase) ? "file:" : null;
                if (prefix is null)
                    continue;
                var value = argument.Length > 2 ? argument[2..] : ++i < count ? ReadArg(argv, i) : "";
                var candidate = NormalizeBase(prefix, value);
                if (candidate is null || (baseId is not null && baseId != candidate))
                    return (mode, null);
                baseId = candidate;
            }
            return (mode, baseId);
        }
        finally
        {
            _ = LocalFree(argv);
        }
    }

    private static string? NormalizeBase(string prefix, string value)
    {
        if (value.Length == 0 || value.StartsWith('/') || value.StartsWith('-') ||
            value.IndexOfAny(['"', '|', '<', '>', '*']) >= 0 ||
            (prefix == "server:" && value.IndexOfAny(['@', ';', '=', '?']) >= 0))
            return null;
        try
        {
            return ThemeConfiguration.NormalizeBaseId(prefix + value);
        }
        catch (Exception error) when (error is InvalidDataException or ArgumentException or IOException or NotSupportedException)
        {
            return null;
        }
    }

    private static string ReadArg(IntPtr argv, int index) =>
        Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv, index * IntPtr.Size)) ?? "";

    [DllImport("shell32.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CommandLineToArgvW(string commandLine, out int argc);

    [DllImport("kernel32.dll", ExactSpelling = true)]
    private static extern IntPtr LocalFree(IntPtr allocated);
}

internal sealed class ProcessDiscovery
{
    private const uint QueryLimitedInformation = 0x1000;
    private const int ProcessCommandLineInformation = 60;
    private const int MaxCommandLineBytes = 65536 + 32;
    private const ushort I386 = 0x014c;
    private const ushort Amd64 = 0x8664;
    private const ushort Arm64 = 0xaa64;

    private readonly uint _session = GetCurrentSession();
    private readonly Dictionary<string, VersionEntry> _versions = new(StringComparer.OrdinalIgnoreCase);

    private sealed record VersionEntry(long Length, DateTime LastWrite, string Version);

    public IReadOnlyList<DetectedProcess> Scan(int? processId = null)
    {
        var result = new List<DetectedProcess>();
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                if (processId is not null && process.Id != processId)
                    continue;
                var name = process.ProcessName;
                var thinClient = name.Equals("1cv8c", StringComparison.OrdinalIgnoreCase);
                if (!thinClient && !name.Equals("1cv8", StringComparison.OrdinalIgnoreCase))
                    continue;
                var detected = Inspect(process.Id, thinClient);
                if (detected is not null)
                    result.Add(detected);
            }
        }
        result.Sort((left, right) => left.Identity.ProcessId.CompareTo(right.Identity.ProcessId));
        return result.AsReadOnly();
    }

    private DetectedProcess? Inspect(int pid, bool thinClient)
    {
        using var handle = OpenProcess(QueryLimitedInformation, false, pid);
        if (!ProcessIdToSessionId(pid, out var session) || session != _session)
            return null;
        if (handle.IsInvalid || !GetProcessTimes(handle, out var creation, out var exit, out _, out _))
            return new(new ProcessIdentity(pid, 0), thinClient ? "Enterprise" : null, null, null, "access_denied");
        if (exit != 0)
            return null;

        var identity = new ProcessIdentity(pid, creation);
        var image = new StringBuilder(1024);
        var length = (uint)image.Capacity;
        if (!QueryFullProcessImageNameW(handle, 0, image, ref length))
        {
            if (Marshal.GetLastWin32Error() != 122)
                return new(identity, thinClient ? "Enterprise" : null, null, null, "access_denied");
            image = new StringBuilder(32768);
            length = (uint)image.Capacity;
            if (!QueryFullProcessImageNameW(handle, 0, image, ref length))
                return new(identity, thinClient ? "Enterprise" : null, null, null, "access_denied");
        }
        var path = image.ToString();
        var executable = Path.GetFileName(path);
        thinClient = executable.Equals("1cv8c.exe", StringComparison.OrdinalIgnoreCase);
        if (!thinClient && !executable.Equals("1cv8.exe", StringComparison.OrdinalIgnoreCase))
            return null;

        var platform = GetPlatform(handle, path);
        var arguments = ReadArguments(handle, thinClient);
        return new(identity, arguments.Mode, platform, arguments.BaseId);
    }

    private static uint GetCurrentSession() =>
        ProcessIdToSessionId(Environment.ProcessId, out var session) ? session : throw new Win32Exception();

    private string? GetPlatform(SafeProcessHandle handle, string path)
    {
        string? architecture;
        try
        {
            if (!IsWow64Process2(handle, out var machine, out var nativeMachine))
                return null;
            architecture = (machine == 0 ? nativeMachine : machine) switch
            {
                I386 => "x86",
                Amd64 => "x64",
                Arm64 => "arm64",
                _ => null
            };
        }
        catch (EntryPointNotFoundException)
        {
            return null;
        }
        if (architecture is null)
            return null;

        try
        {
            var file = new FileInfo(path);
            var length = file.Length;
            var lastWrite = file.LastWriteTimeUtc;
            if (!_versions.TryGetValue(path, out var entry) || entry.LastWrite != lastWrite || entry.Length != length)
            {
                var version = FileVersionInfo.GetVersionInfo(path);
                if (version.FileMajorPart == 0)
                    return null;
                entry = new VersionEntry(length, lastWrite, new Version(version.FileMajorPart, version.FileMinorPart,
                    version.FileBuildPart, version.FilePrivatePart).ToString(4));
                if (_versions.Count >= 128)
                    _versions.Clear();
                _versions[path] = entry;
            }
            return entry.Version + "-" + architecture;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception)
        {
            return null;
        }
    }

    private static (string? Mode, string? BaseId) ReadArguments(SafeProcessHandle handle, bool thinClient)
    {
        // Class 60 returns a UNICODE_STRING pointing inside the caller's buffer.
        _ = NtQueryInformationProcess(handle, ProcessCommandLineInformation, IntPtr.Zero, 0, out var size);
        var headerSize = Marshal.SizeOf<NativeString>();
        if (size < headerSize || size > MaxCommandLineBytes)
            return (thinClient ? "Enterprise" : null, null);
        var buffer = Marshal.AllocHGlobal(size);
        try
        {
            var status = NtQueryInformationProcess(handle, ProcessCommandLineInformation, buffer, size, out var returned);
            if (status < 0 || returned < headerSize || returned > size)
                return (thinClient ? "Enterprise" : null, null);
            var value = Marshal.PtrToStructure<NativeString>(buffer);
            var offset = value.Buffer.ToInt64() - buffer.ToInt64();
            if (value.Length == 0 || value.Length % 2 != 0 || value.Length > value.MaximumLength ||
                offset < headerSize || offset > returned - value.Length)
                return (thinClient ? "Enterprise" : null, null);
            return OneCArguments.Parse(Marshal.PtrToStringUni(value.Buffer, value.Length / 2) ?? "", thinClient);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeString
    {
        public ushort Length;
        public ushort MaximumLength;
        public IntPtr Buffer;
    }

    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    private static extern SafeProcessHandle OpenProcess(uint desiredAccess, bool inheritHandle, int processId);

    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    private static extern bool ProcessIdToSessionId(int processId, out uint sessionId);

    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    private static extern bool GetProcessTimes(SafeProcessHandle process, out ulong creation, out ulong exit,
                                               out ulong kernel, out ulong user);

    [DllImport("kernel32.dll", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageNameW(SafeProcessHandle process, uint flags,
                                                          StringBuilder path, ref uint size);

    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    private static extern bool IsWow64Process2(SafeProcessHandle process, out ushort processMachine, out ushort nativeMachine);

    [DllImport("ntdll.dll", ExactSpelling = true)]
    private static extern int NtQueryInformationProcess(SafeProcessHandle process, int informationClass,
                                                        IntPtr information, int length, out int returnLength);
}
