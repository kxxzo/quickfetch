## quickfetch — a small fastfetch-like system info tool written in Nim.

import std/[os, osproc, strutils, strformat, tables]

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------

const
  reset      = "\e[0m"
  bold       = "\e[1m"
  foreground = "\e[0m"
  red        = "\e[31m"
  green      = "\e[32m"
  yellow     = "\e[33m"
  blue       = "\e[34m"
  magenta    = "\e[35m"
  cyan       = "\e[36m"


  terminalColors = [
    "\e[90m", "\e[37m", "\e[36m", "\e[35m", "\e[34m", "\e[33m", "\e[32m", "\e[31m",
  ]

# ---------------------------------------------------------------------------
# Appearence
# ---------------------------------------------------------------------------

const
  logo = @[
    "           +           ",
    "         +++++         ",
    "  +      +++++      +  ",
    "++++++    +++    ++++++",
    " +++++++  +++  +++++++ ",
    "     +++++++++++++     ",
    "        +++++++        ",
    "     +++++++++++++     ",
    " +++++++  +++  +++++++ ",
    "++++++    +++    ++++++",
    "  +      +++++      +  ",
    "         +++++         ",
    "           +           ",
  ]
  logoColor = cyan

  iconOs = ""
  iconOsColor = blue

  iconShell = ""
  iconShellColor = blue

  iconTerminal = ""
  iconTerminalColor = blue

  iconCpu = ""
  iconCpuColor = blue

  iconGpu = "󰢮"
  iconGpuColor = blue

  iconMemory = "󱟱"
  iconMemoryColor = blue

  titleColor = cyan
  colorCharacter = "●"
  separatorColor = foreground

# ---------------------------------------------------------------------------
# Modules order
# ---------------------------------------------------------------------------

type
  Module = enum
    mTitle, mSeparator, mOs, mShell, mTerminal, mCpu, mGpu, mMemory, mBreak, mColors

const modulesOrder = @[
  mTitle,
  mSeparator,
  mOs,
  mShell,
  mTerminal,
  mCpu,
  mGpu,
  mMemory,
  mBreak,
  mColors,
]

# ---------------------------------------------------------------------------
# Get functions — return plain data, no coloring/printing
# ---------------------------------------------------------------------------

proc getUserHost(): string =
  let user = getEnv("USER", "user")
  var host = ""
  try:
    host = readFile("/etc/hostname").strip()
  except IOError:
    host = getEnv("HOSTNAME", "host")
  result = &"{user}@{host}"
 
proc getOS(cache: var Table[string, string]): string =
  if cache.hasKey("os"):
    return cache["os"]
  try:
    for line in lines("/etc/os-release"):
      if line.startsWith("PRETTY_NAME="):
        result = line.split('=', 1)[1].strip(chars = {'"'})
        cache["os"] = result
        return
  except IOError:
    discard
  result = "Unknown"
  cache["os"] = result
 
proc getShell(): string =
  let shellPath = getEnv("SHELL", "unknown")
  let name = shellPath.extractFilename()
  # Shells export their own version, so read it instead of forking
  # a "<shell> --version" subprocess (which costs several ms just
  # for the fork/exec, dwarfing everything else this tool does).
  var version = case name
    of "bash": getEnv("BASH_VERSION").split('(')[0].strip()
    of "zsh": getEnv("ZSH_VERSION")
    of "fish": getEnv("FISH_VERSION")
    else: ""
  if version.len == 0:
    # Uncommon shell with no known version env var — fall back to
    # spawning it, since we have no other cheap way to get this.
    try:
      let (output, code) = execCmdEx(shellPath & " --version")
      if code == 0 and output.len > 0:
        let firstLine = output.splitLines()[0]
        for token in firstLine.splitWhitespace():
          if token.len > 0 and token[0].isDigit():
            version = token
            break
    except OSError:
      discard
  result = if version.len > 0: &"{name} {version}" else: name
 
proc getTerminal(): string =
  var term = getEnv("TERM_PROGRAM", "")
  if term.len == 0:
    # Walk up the process tree looking for the first non-shell ancestor.
    var pid = getCurrentProcessId()
    for _ in 0 ..< 10:
      let statPath = &"/proc/{pid}/stat"
      if not fileExists(statPath):
        break
      let stat = readFile(statPath)
      let closeParen = stat.rfind(')')
      if closeParen < 0:
        break
      let fields = stat[closeParen + 2 .. ^1].splitWhitespace()
      if fields.len < 2:
        break
      let ppid = fields[1]
      let commPath = &"/proc/{ppid}/comm"
      if not fileExists(commPath):
        break
      let comm = readFile(commPath).strip()
      if comm notin ["bash", "zsh", "fish", "sh", "quickfetch"]:
        term = comm
        break
      pid = parseInt(ppid)
  result = if term.len > 0: term else: "unknown"
 
proc cleanCpuModel(raw: string): string =
  ## Strips vendor/marketing cruft fastfetch-style, e.g. turns
  ## "AMD Ryzen 5 7500F 6-Core Processor" into "AMD Ryzen 5 7500F", and
  ## "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz" into "Intel Core i7-9700K".
  var model = raw.replace("(R)", "").replace("(TM)", "").replace("(C)", "")
  let atIdx = model.find(" @")
  if atIdx >= 0:
    model = model[0 ..< atIdx]
  let tokens = model.splitWhitespace()
  var cutAt = tokens.len
  for i, tok in tokens:
    let lower = tok.toLowerAscii()
    if lower == "cpu" or lower.endsWith("-core"):
      cutAt = i
      break
  result = tokens[0 ..< cutAt].join(" ").strip()
 
proc cleanGpuModel(raw: string): string =
  ## lspci reports e.g. "NVIDIA Corporation AD104 [GeForce RTX 4070 Ti]
  ## (rev a1)" — pull out the product name from the last bracket pair.
  let closeB = raw.rfind(']')
  if closeB > 0:
    let openB = raw[0 ..< closeB].rfind('[')
    if openB >= 0:
      return raw[openB + 1 ..< closeB]
  result = raw.strip()
 
proc getCpu(cache: var Table[string, string]): string =
  if cache.hasKey("cpu"):
    return cache["cpu"]
  try:
    for line in lines("/proc/cpuinfo"):
      if line.startsWith("model name"):
        result = cleanCpuModel(line.split(':', 1)[1].strip())
        cache["cpu"] = result
        return
  except IOError:
    discard
  result = "Unknown CPU"
  cache["cpu"] = result
 
const pciIdsPaths = [
  "/usr/share/hwdata/pci.ids",
  "/usr/share/misc/pci.ids",
  "/usr/share/pci.ids",
]

const cacheDir = getHomeDir() / ".cache/quickfetch"
const cachePath = cacheDir / "cache"

proc loadCache(): Table[string, string] =
  ## Cache format: one "key=value" per line. Read once per run.
  result = initTable[string, string]()
  try:
    for line in lines(cachePath):
      let idx = line.find('=')
      if idx > 0:
        result[line[0 ..< idx]] = line[idx + 1 .. ^1]
  except IOError:
    discard

proc saveCache(cache: Table[string, string]) =
  ## Written once per run, after any new values were computed.
  try:
    createDir(cacheDir)
    var lines: seq[string]
    for key, value in cache:
      lines.add(&"{key}={value}")
    writeFile(cachePath, lines.join("\n"))
  except IOError:
    discard

proc lookupPciNames(vendorId, deviceId: string): tuple[vendor, device: string] =
  ## Parses pci.ids to resolve a vendor:device pair to human-readable names,
  ## without spawning lspci. Format is tab-indented:
  ##   vendorId  vendorName
  ##   \tdeviceId  deviceName
  for path in pciIdsPaths:
    if not fileExists(path):
      continue
    try:
      var inVendor = false
      for line in lines(path):
        if line.len == 0 or line[0] == '#':
          continue
        if line[0] != '\t':
          # top-level vendor line
          let parts = line.splitWhitespace(maxsplit = 1)
          if parts.len == 2 and parts[0].toLowerAscii() == vendorId:
            result.vendor = parts[1]
            inVendor = true
          else:
            inVendor = false
        elif inVendor and line.len > 1 and line[1] != '\t':
          let parts = line[1 .. ^1].splitWhitespace(maxsplit = 1)
          if parts.len == 2 and parts[0].toLowerAscii() == deviceId:
            result.device = parts[1]
            return
      if result.vendor.len > 0:
        return
    except IOError:
      discard

proc getGpu(cache: var Table[string, string]): string =
  const pciDevicesDir = "/sys/bus/pci/devices"
  try:
    for kind, path in walkDir(pciDevicesDir):
      if kind != pcDir and kind != pcLinkToDir:
        continue
      let classPath = path / "class"
      if not fileExists(classPath):
        continue
      let classId = readFile(classPath).strip()
      # 0x03xxxx = display controller (VGA/3D/other)
      if not classId.startsWith("0x03"):
        continue
      let vendorPath = path / "vendor"
      let devicePath = path / "device"
      if not fileExists(vendorPath) or not fileExists(devicePath):
        continue
      let vendorId = readFile(vendorPath).strip().replace("0x", "")
      let deviceId = readFile(devicePath).strip().replace("0x", "")

      if cache.hasKey("gpu"):
        let parts = cache["gpu"].split(':', 2)
        if parts.len == 3 and parts[0] == vendorId and parts[1] == deviceId:
          return parts[2]

      let (vendor, device) = lookupPciNames(vendorId, deviceId)
      if device.len > 0:
        let name = cleanGpuModel(device)
        cache["gpu"] = &"{vendorId}:{deviceId}:{name}"
        return name
      elif vendor.len > 0:
        cache["gpu"] = &"{vendorId}:{deviceId}:{vendor}"
        return vendor
  except OSError:
    discard
  result = "Unknown GPU"
 
proc getMemory(cache: var Table[string, string]): tuple[totalGiB: float, usedPercent: int] =
  var totalKb, availKb: int
  var haveTotal, haveAvail = false

  let cachedTotalKb = cache.getOrDefault("ram_total_kb", "")
  if cachedTotalKb.len > 0:
    totalKb = cachedTotalKb.parseInt()
    haveTotal = true

  try:
    for line in lines("/proc/meminfo"):
      if not haveTotal and line.startsWith("MemTotal:"):
        totalKb = line.splitWhitespace()[1].parseInt()
        haveTotal = true
        cache["ram_total_kb"] = $totalKb
      elif line.startsWith("MemAvailable:"):
        availKb = line.splitWhitespace()[1].parseInt()
        haveAvail = true
      if haveTotal and haveAvail:
        break
  except IOError:
    discard
  let totalGiB = totalKb.float / 1024.0 / 1024.0
  let usedPercent =
    if totalKb > 0: int(((totalKb - availKb).float / totalKb.float) * 100.0)
    else: 0
  result = (totalGiB, usedPercent)
 
# ---------------------------------------------------------------------------
# Print Function — all coloring and output happens here
# ---------------------------------------------------------------------------
 
proc colorForPercent(p: int): string =
  if p < 50: green
  elif p < 80: yellow
  else: red
 
proc renderInfoLines(): seq[string] =
  var cache = loadCache()

  let userHost = getUserHost()
  let separator = "-".repeat(userHost.len)
  let os = getOS(cache)
  let shell = getShell()
  let terminal = getTerminal()
  let cpu = getCpu(cache)
  let gpu = getGpu(cache)
  let (totalGiB, usedPercent) = getMemory(cache)

  saveCache(cache)
 
  for m in modulesOrder:
    case m
    of mTitle:
      let atIdx = userHost.find('@')
      if atIdx >= 0:
        let userPart = userHost[0 ..< atIdx]
        let hostPart = userHost[atIdx + 1 .. ^1]
        result.add(&"{bold}{titleColor}{userPart}{foreground}@{bold}{titleColor}{hostPart}{reset}")
      else:
        result.add(&"{bold}{titleColor}{userHost}{reset}")
    of mSeparator:
      result.add(&"{separatorColor}{separator}{reset}")
    of mOs:
      result.add(&"{iconOsColor}{iconOs}{reset}  {os}")
    of mShell:
      result.add(&"{iconShellColor}{iconShell}{reset}  {shell}")
    of mTerminal:
      result.add(&"{iconTerminalColor}{iconTerminal}{reset}  {terminal}")
    of mCpu:
      result.add(&"{iconCpuColor}{iconCpu}{reset}  {cpu}")
    of mGpu:
      result.add(&"{iconGpuColor}{iconGpu}{reset}  {gpu}")
    of mMemory:
      let pctColor = colorForPercent(usedPercent)
      result.add(&"{iconMemoryColor}{iconMemory}{reset}  {totalGiB:.2f} GiB {pctColor}{usedPercent}%{reset}")
    of mBreak:
      result.add("")
    of mColors:
      var dots: seq[string]
      for c in terminalColors:
        dots.add(&"{c}{colorCharacter}{reset}")
      result.add(dots.join(" "))
 
proc printFetch() =
  let infoLines = renderInfoLines()
  let lineCount = max(logo.len, infoLines.len)
 
  for i in 0 ..< lineCount:
    let logoLine =
      if i < logo.len: &"{bold}{logoColor}{logo[i]}{reset}"
      else: " ".repeat(logo[0].len)
    let infoLine = if i < infoLines.len: infoLines[i] else: ""
    echo &"{logoLine}  {infoLine}"
 
when isMainModule:
  # Minimal flag check — paramCount/paramStr are just reads of already-parsed
  # argv, so this is far cheaper than pulling in std/parseopt for one flag.
  if paramCount() >= 1 and paramStr(1) == "--refresh":
    try:
      removeFile(cachePath)
    except OSError:
      discard
  printFetch()
