// Show today's date (browser local date) since ESP32 timestamp is "millis since boot"
function formatDateForHeader() {
  const now = new Date();
  const options = { weekday: "long", year: "numeric", month: "long", day: "numeric" };
  return now.toLocaleDateString(undefined, options);
}

// Convert mm -> inches
function mmToIn(mm) {
  return mm / 25.4;
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
        const inches = mmToIn(node.distance);
        statusEl.textContent = `${inches.toFixed(3)} in`;
      }
    }
    return;
  }

  // Fallback (if you ever feed old demo JSON again)
  document.getElementById("status1").textContent = (packet.distance1 ?? "NO DATA") + " in";
  document.getElementById("status2").textContent = (packet.distance2 ?? "NO DATA") + " in";
  document.getElementById("status3").textContent = (packet.distance3 ?? "NO DATA") + " in";
  document.getElementById("status4").textContent = (packet.distance4 ?? "NO DATA") + " in";
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
  console.error("Polling error:", err);
  document.getElementById("status1").textContent = "DISCONNECTED";
  document.getElementById("status2").textContent = "DISCONNECTED";
  document.getElementById("status3").textContent = "DISCONNECTED";
  document.getElementById("status4").textContent = "DISCONNECTED";
  }
}

// Start polling when page loads
setInterval(pollData, POLL_INTERVAL_MS);
pollData(); // run immediately once




// CLOCK

// Function to show the current time at the top-left corner
function startClock() {
  const clock = document.getElementById("clock");

  function updateTime() {
    const now = new Date();
    const options = { hour: "numeric", minute: "numeric", second: "numeric" };
    clock.textContent = now.toLocaleTimeString(undefined, options);
  }

  updateTime();             // show immediately
  setInterval(updateTime, 1000); // update every second
}

// Start the clock when the page loads
startClock();


