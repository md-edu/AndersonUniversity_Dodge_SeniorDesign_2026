// Show today's date (browser local date) since ESP32 timestamp is "millis since boot"
function formatDateForHeader() {
  const now = new Date();
  const options = { weekday: "long", year: "numeric", month: "long", day: "numeric" };
  return now.toLocaleDateString(undefined, options);
}

// Update the page using ESP32's JSON format: { timestamp, uptime, nodes: [...] }
function updateDisplay(packet) {
  // Update header date (local date)
  document.getElementById("datetime").textContent = formatDateForHeader();

  // If firmware returns nodes[], use it
  if (packet.nodes && Array.isArray(packet.nodes)) {
    for (let sensorNum = 1; sensorNum <= 4; sensorNum++) {
      const nodeId = sensorNum - 1; // sensor1 -> node 0
      const node = packet.nodes.find(n => n.id === nodeId);
      const statusEl = document.getElementById(`status${sensorNum}`);

      if (!node || !node.online) {
        statusEl.textContent = "NO SIGNAL";
      } else {
        statusEl.textContent = `${node.distance.toFixed(2)} mm`;
      }
    }
    return;
  }

  // Fallback (if you ever feed old demo JSON again)
  document.getElementById("status1").textContent = (packet.distance1 ?? "NO DATA") + " mm";
  document.getElementById("status2").textContent = (packet.distance2 ?? "NO DATA") + " mm";
  document.getElementById("status3").textContent = (packet.distance3 ?? "NO DATA") + " mm";
  document.getElementById("status4").textContent = (packet.distance4 ?? "NO DATA") + " mm";
}

// POLLING CODE
const POLL_INTERVAL_MS = 1000; // browser asks server for data

async function pollData() {
  try {
    const res = await fetch("/api/data", { cache: "no-store" });
    // send HTTP GET request to /api/data, pause until server responds
    // no store prevents browser from reusing cached responses
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json(); // convert JSON string from server into JavaScript object
    console.log("Received data:", data);
    updateDisplay(data);
  } catch (err) {
    console.error("Poll error:", err);
  }
}

// Start polling
setInterval(pollData, POLL_INTERVAL_MS);
pollData(); // first call immediately

// Clock
function updateClock() {
  const now = new Date();
  document.getElementById("clock").textContent = now.toLocaleTimeString();
}
setInterval(updateClock, 1000);
updateClock();
