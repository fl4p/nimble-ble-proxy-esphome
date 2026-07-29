import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.6.0/bundle.js";

const repo = "fl4p/nimble-ble-proxy-esphome";
const releasesUrl = `https://api.github.com/repos/${repo}/releases`;
const releasePageUrl = `https://github.com/${repo}/releases`;
const supportedTargets = ["esp32", "esp32s3", "esp32c3", "esp32c6"];

const el = (id) => document.getElementById(id);
const releaseSelect = el("releaseSelect");
const variantSelect = el("variantSelect");
const baudSelect = el("baudSelect");
const wifiSsid = el("wifiSsid");
const wifiPsk = el("wifiPsk");
const connectButton = el("connectButton");
const flashButton = el("flashButton");
const support = el("support");
const chipStatus = el("chipStatus");
const assetStatus = el("assetStatus");
const progress = el("progress");
const progressText = el("progressText");
const logEl = el("log");
const releasesEl = el("releases");

let releases = [];
let port = null;
let transport = null;
let loader = null;
let detectedTarget = null;
let detectedChipName = null;
let serialMonitorActive = false;
let serialMonitorReader = null;

const terminal = {
  clean() {
    logEl.textContent = "";
  },
  writeLine(data) {
    appendLog(data);
  },
  write(data) {
    logEl.textContent += data;
    logEl.scrollTop = logEl.scrollHeight;
  },
};

function appendLog(line = "") {
  logEl.textContent += `${line}\n`;
  logEl.scrollTop = logEl.scrollHeight;
}

function setSupportMessage(message, className = "support ok") {
  support.className = className;
  support.classList.toggle("hidden", !message);
  support.textContent = message;
}

function setProgress(value) {
  progress.value = value;
  progressText.textContent = `${Math.round(value)}%`;
}

function formatSerialPortInfo(selectedPort) {
  const info = selectedPort.getInfo();
  const vendor = info.usbVendorId === undefined ? "unknown" : `0x${info.usbVendorId.toString(16).padStart(4, "0")}`;
  const product = info.usbProductId === undefined ? "unknown" : `0x${info.usbProductId.toString(16).padStart(4, "0")}`;
  return `USB vendor ${vendor}, product ${product}`;
}

function normalizeTarget(chipName) {
  const normalized = chipName.toLowerCase().replace(/[^a-z0-9]/g, "");
  if (normalized.includes("esp32c6")) return "esp32c6";
  if (normalized.includes("esp32c3")) return "esp32c3";
  if (normalized.includes("esp32s3")) return "esp32s3";
  if (normalized.includes("esp32")) return "esp32";
  return null;
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) return "";
  if (bytes < 1024 * 1024) return `${Math.round(bytes / 1024)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function byteLength(value) {
  return new TextEncoder().encode(value).length;
}

function getWifiCredentials() {
  const ssid = wifiSsid.value;
  const psk = wifiPsk.value;
  if (!ssid) return null;
  const pskBytes = byteLength(psk);
  if (byteLength(ssid) > 32) throw new Error("WiFi SSID must be 32 bytes or less");
  if (pskBytes > 0 && pskBytes < 8) throw new Error("WiFi password must be empty for open networks or at least 8 bytes");
  if (pskBytes > 64) throw new Error("WiFi password must be 64 bytes or less");
  if (pskBytes === 64 && !/^[0-9a-fA-F]{64}$/.test(psk)) throw new Error("64-byte WiFi passwords must be hex PSKs");
  return { ssid, psk };
}

function base64Token(value) {
  if (!value) return "-";
  const bytes = new TextEncoder().encode(value);
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

function selectedRelease() {
  return releases.find((release) => release.id.toString() === releaseSelect.value) || releases[0];
}

function findFlashAsset(target = detectedTarget) {
  const release = selectedRelease();
  if (!release || !target) return null;
  const variant = variantSelect.value;
  const exactName = `nimble_ble_proxy-${target}-${variant}-flash.bin`;
  const legacyS3Name = `nimble_ble_proxy-${variant}-flash.bin`;
  return release.assets.find((asset) => asset.name === exactName)
    || (target === "esp32s3" ? release.assets.find((asset) => asset.name === legacyS3Name) : null)
    || release.assets.find((asset) => asset.name.includes(target) && asset.name.includes(variant) && asset.name.endsWith("-flash.bin"));
}

function updateSelectedAsset() {
  const asset = findFlashAsset();
  if (!detectedTarget) {
    assetStatus.textContent = "connect a board first";
    flashButton.disabled = true;
    return;
  }
  if (!asset) {
    assetStatus.textContent = `no ${detectedTarget}/${variantSelect.value} flash image in selected release`;
    flashButton.disabled = true;
    return;
  }
  assetStatus.textContent = `${asset.name} (${formatBytes(asset.size)})`;
  flashButton.disabled = false;
}

function renderReleaseOptions() {
  releaseSelect.innerHTML = "";
  for (const release of releases) {
    const option = document.createElement("option");
    option.value = release.id;
    option.textContent = `${release.tag_name}${release.prerelease ? " (pre-release)" : ""}`;
    releaseSelect.appendChild(option);
  }
}

function renderReleases() {
  if (!releases.length) {
    releasesEl.innerHTML = `No releases found. <a href="${releasePageUrl}">Open GitHub releases</a>.`;
    return;
  }

  releasesEl.innerHTML = "";
  for (const release of releases) {
    const item = document.createElement("article");
    item.className = "release";

    const title = document.createElement("h3");
    const titleLink = document.createElement("a");
    titleLink.href = release.html_url;
    titleLink.target = "_blank";
    titleLink.rel = "noreferrer";
    titleLink.textContent = release.name || release.tag_name;
    title.appendChild(titleLink);
    item.appendChild(title);

    const meta = document.createElement("small");
    meta.textContent = `${release.tag_name} · ${new Date(release.published_at || release.created_at).toLocaleString()} · ${release.assets.length} assets`;
    item.appendChild(meta);

    const assets = document.createElement("div");
    assets.className = "assets";
    for (const asset of release.assets.filter((candidate) => candidate.name.endsWith(".bin") || candidate.name.startsWith("flash_args"))) {
      const link = document.createElement("a");
      link.className = "asset";
      link.href = asset.browser_download_url;
      link.textContent = `${asset.name} (${formatBytes(asset.size)})`;
      assets.appendChild(link);
    }
    item.appendChild(assets);
    releasesEl.appendChild(item);
  }
}

async function loadReleases() {
  releasesEl.textContent = "Loading releases…";
  releaseSelect.disabled = true;
  const response = await fetch(releasesUrl, { headers: { Accept: "application/vnd.github+json" } });
  if (!response.ok) throw new Error(`GitHub releases API returned ${response.status}`);
  releases = await response.json();
  renderReleaseOptions();
  renderReleases();
  releaseSelect.disabled = false;
  updateSelectedAsset();
}

function checkSupport() {
  if (!isSecureContext) {
    setSupportMessage("Web Serial requires HTTPS or localhost.", "support bad");
    return false;
  }
  if (!("serial" in navigator)) {
    setSupportMessage("This browser does not support Web Serial. Use Chrome or Edge.", "support bad");
    return false;
  }
  setSupportMessage("");
  return true;
}

async function getSerialPort() {
  const grantedPorts = await navigator.serial.getPorts();
  if (grantedPorts.length === 1) return grantedPorts[0];
  return navigator.serial.requestPort({});
}

function setConnectionButtonConnected(connected) {
  connectButton.textContent = connected ? "Disconnect" : "Connect";
  connectButton.classList.toggle("primary", !connected);
  connectButton.classList.toggle("danger", connected);
}

async function connectAndDetect() {
  if (!checkSupport()) return;
  connectButton.disabled = true;
  setProgress(0);
  try {
    port = await getSerialPort();
    transport = new Transport(port, true);
    loader = new ESPLoader({
      transport,
      baudrate: Number(baudSelect.value),
      terminal,
      debugLogging: false,
    });
    appendLog(`Selected serial port: ${formatSerialPortInfo(port)}`);
    detectedChipName = await loader.main();
    detectedTarget = normalizeTarget(detectedChipName || loader.chip?.CHIP_NAME || "");
    if (!supportedTargets.includes(detectedTarget)) {
      throw new Error(`Detected unsupported chip: ${detectedChipName}`);
    }
    chipStatus.textContent = `${detectedChipName} → ${detectedTarget}`;
    appendLog(`Detected target: ${detectedTarget}`);
    setConnectionButtonConnected(true);
    updateSelectedAsset();
  } catch (error) {
    chipStatus.textContent = "not connected";
    appendLog(`Error: ${error.message || error}`);
    if (!detectedChipName) {
      setSupportMessage("Could not detect chip. Hold BOOT while connecting if your board does not enter download mode automatically.", "support bad");
    }
    await disconnect();
  } finally {
    connectButton.disabled = false;
  }
}

function hostedFirmwareUrl(asset) {
  const release = selectedRelease();
  return `firmware/${encodeURIComponent(release.tag_name)}/${encodeURIComponent(asset.name)}`;
}

async function downloadAsset(asset) {
  const url = hostedFirmwareUrl(asset);
  appendLog(`Downloading ${url}…`);
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Download failed with HTTP ${response.status}: ${url}`);
  return new Uint8Array(await response.arrayBuffer());
}

async function stopSerialMonitor() {
  serialMonitorActive = false;
  if (serialMonitorReader) {
    try { await serialMonitorReader.cancel(); } catch (_error) {}
  }
}

async function startSerialMonitor() {
  if (!port) return;
  appendLog("Watching serial output for the device IP address. Click Disconnect to stop.");
  chipStatus.textContent = "serial monitor";
  assetStatus.textContent = "serial monitor active";
  flashButton.disabled = true;
  setConnectionButtonConnected(true);

  const deadline = Date.now() + 15000;
  while (!port.readable && Date.now() < deadline) {
    try {
      await port.open({ baudRate: 115200 });
    } catch (_error) {
      await sleep(300);
    }
  }
  if (!port.readable) {
    appendLog("Serial monitor could not reopen after reset.");
    return;
  }

  serialMonitorActive = true;
  const decoder = new TextDecoder();
  const reader = port.readable.getReader();
  serialMonitorReader = reader;
  try {
    while (serialMonitorActive) {
      const result = await reader.read();
      if (result.done) break;
      terminal.write(decoder.decode(result.value, { stream: true }));
    }
  } catch (error) {
    if (serialMonitorActive) appendLog(`Serial monitor stopped: ${error.message || error}`);
  } finally {
    serialMonitorActive = false;
    if (serialMonitorReader === reader) serialMonitorReader = null;
    reader.releaseLock();
  }
}

async function provisionWifiOverSerial(credentials) {
  if (!credentials) return;

  const line = `NBP-PROV1 ${base64Token(credentials.ssid)} ${base64Token(credentials.psk)}\n`;
  appendLog("Provisioning WiFi over serial…");

  const deadline = Date.now() + 12000;
  while (Date.now() < deadline) {
    try {
      if (!port.readable || !port.writable) {
        await port.open({ baudRate: 115200 });
      }
      break;
    } catch (error) {
      await sleep(300);
    }
  }
  if (!port.writable || !port.readable) throw new Error("serial port did not reopen for WiFi provisioning");

  await port.setSignals({ dataTerminalReady: false, requestToSend: true });
  await sleep(100);
  await port.setSignals({ requestToSend: false });
  await sleep(250);

  const writer = port.writable.getWriter();
  const reader = port.readable.getReader();
  const decoder = new TextDecoder();
  let closed = false;
  let received = "";
  const readLoop = (async () => {
    try {
      while (!closed) {
        const result = await reader.read();
        if (result.done) break;
        received += decoder.decode(result.value, { stream: true });
      }
    } catch (_error) {
      // Reset after successful provisioning can close the port while reading.
    }
  })();

  try {
    while (Date.now() < deadline) {
      await writer.write(new TextEncoder().encode(line));
      await sleep(500);
      if (received.includes("NBP-PROV-OK")) {
        appendLog("WiFi credentials stored. Device rebooting…");
        return;
      }
    }
    throw new Error("device did not acknowledge WiFi provisioning");
  } finally {
    closed = true;
    try { await reader.cancel(); } catch (_error) {}
    try { await readLoop; } catch (_error) {}
    reader.releaseLock();
    writer.releaseLock();
  }
}

async function flashDetectedChip() {
  const asset = findFlashAsset();
  if (!loader || !asset) return;

  let credentials = null;
  try {
    credentials = getWifiCredentials();
  } catch (error) {
    appendLog(`Error: ${error.message || error}`);
    return;
  }

  const provisioningNote = credentials ? "\n\nWiFi credentials will be provisioned after flashing." : "";
  const confirmed = confirm(`Flash ${asset.name} to ${detectedChipName}?\n\nThis erases the app currently on the device.${provisioningNote}`);
  if (!confirmed) return;

  flashButton.disabled = true;
  connectButton.disabled = true;
  setProgress(0);
  try {
    const data = await downloadAsset(asset);
    appendLog(`Writing ${formatBytes(data.byteLength)} at 0x0…`);
    await loader.writeFlash({
      fileArray: [{ data, address: 0x0 }],
      flashMode: "dio",
      flashFreq: "80m",
      flashSize: "4MB",
      eraseAll: false,
      compress: true,
      reportProgress: (_fileIndex, written, total) => setProgress((written / total) * 100),
    });
    appendLog("Flashing complete. Resetting…");
    await loader.after("hard_reset");
    await transport.disconnect();
    transport = null;
    loader = null;
    await provisionWifiOverSerial(credentials);
    if (credentials) {
      appendLog("Flash and WiFi provisioning complete.");
      detectedTarget = null;
      detectedChipName = null;
      void startSerialMonitor();
    } else {
      appendLog("Done.");
      await disconnect();
    }
  } catch (error) {
    appendLog(`Error: ${error.message || error}`);
  } finally {
    flashButton.disabled = false;
    connectButton.disabled = false;
  }
}

async function disconnect() {
  await stopSerialMonitor();
  try {
    if (transport) await transport.disconnect();
  } catch (error) {
    appendLog(`Disconnect error: ${error.message || error}`);
  }
  try {
    if (port && port.readable) await port.close();
  } catch (error) {
    appendLog(`Serial close error: ${error.message || error}`);
  }
  port = null;
  transport = null;
  loader = null;
  detectedTarget = null;
  detectedChipName = null;
  chipStatus.textContent = "not connected";
  setConnectionButtonConnected(false);
  updateSelectedAsset();
}

async function toggleConnection() {
  if (transport) {
    await disconnect();
    return;
  }
  await connectAndDetect();
}

connectButton.addEventListener("click", toggleConnection);
flashButton.addEventListener("click", flashDetectedChip);
releaseSelect.addEventListener("change", updateSelectedAsset);
variantSelect.addEventListener("change", updateSelectedAsset);

checkSupport();
loadReleases().catch((error) => {
  releasesEl.textContent = `Failed to load releases: ${error.message || error}`;
  assetStatus.textContent = "release metadata failed to load";
});
