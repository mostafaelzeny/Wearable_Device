#ifndef DASHBOARD_PAGE_H
#define DASHBOARD_PAGE_H

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 AD5933 Dashboard</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    :root {
      color-scheme: light;
      font-family: Arial, Helvetica, sans-serif;
      background: #f5f7fb;
      color: #172033;
    }
    body {
      margin: 0;
      padding: 18px;
    }
    main {
      max-width: 1180px;
      margin: 0 auto;
    }
    h1 {
      margin: 0 0 14px;
      font-size: 28px;
    }
    .toolbar {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-bottom: 14px;
    }
    button, a.button {
      border: 1px solid #244a8f;
      background: #244a8f;
      color: white;
      border-radius: 6px;
      padding: 10px 14px;
      font-size: 15px;
      cursor: pointer;
      text-decoration: none;
      line-height: 1.2;
    }
    button.secondary, a.button.secondary {
      background: white;
      color: #244a8f;
    }
    input[type="file"] {
      display: none;
    }
    #status {
      min-height: 22px;
      margin: 8px 0 16px;
      color: #344054;
      font-weight: 600;
    }
    h2 {
      margin: 20px 0 10px;
      font-size: 20px;
    }
    .charts {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
      gap: 16px;
      margin-bottom: 18px;
    }
    .chartPanel {
      background: white;
      border: 1px solid #dde4f0;
      border-radius: 8px;
      padding: 12px;
      min-height: 310px;
    }
    .tableWrap {
      overflow-x: auto;
      background: white;
      border: 1px solid #dde4f0;
      border-radius: 8px;
    }
    .sessionList {
      display: grid;
      gap: 8px;
      margin-bottom: 16px;
    }
    .sessionItem {
      display: grid;
      grid-template-columns: minmax(220px, 1fr) auto auto auto;
      gap: 8px;
      align-items: center;
      background: white;
      border: 1px solid #dde4f0;
      border-radius: 8px;
      padding: 10px;
    }
    .sessionName {
      overflow-wrap: anywhere;
      font-family: Consolas, monospace;
      font-size: 13px;
    }
    .sessionItem button, .sessionItem a.button {
      padding: 8px 10px;
      font-size: 13px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      min-width: 860px;
    }
    th, td {
      padding: 8px 10px;
      border-bottom: 1px solid #edf1f7;
      text-align: right;
      white-space: nowrap;
    }
    th {
      background: #eef3fb;
      color: #172033;
      font-size: 13px;
    }
    th:first-child, td:first-child {
      text-align: left;
    }
    @media (max-width: 640px) {
      body {
        padding: 12px;
      }
      h1 {
        font-size: 22px;
      }
      button, a.button {
        width: 100%;
      }
      .sessionItem {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <main>
    <h1>ESP32 AD5933 Impedance Dashboard</h1>
    <div class="toolbar">
      <button onclick="runAction('/calibrate', 'Calibration running...')">Start Calibration</button>
      <button onclick="runAction('/measure', 'Measurement running...')">Start Measurement</button>
      <button class="secondary" onclick="refreshDashboard()">Refresh Data</button>
      <button class="secondary" onclick="document.getElementById('csvImport').click()">Upload Saved CSV</button>
      <input id="csvImport" type="file" accept=".csv,text/csv" onchange="uploadSavedCsv(event)">
      <a class="button secondary" href="/csv">Download CSV</a>
    </div>
    <div id="status">Ready</div>

    <h2>Saved Sessions</h2>
    <div id="sessionList" class="sessionList"></div>

    <section class="charts">
      <div class="chartPanel"><canvas id="zChart"></canvas></div>
      <div class="chartPanel"><canvas id="phaseChart"></canvas></div>
      <div class="chartPanel"><canvas id="reactanceChart"></canvas></div>
      <div class="chartPanel"><canvas id="resistanceReactanceChart"></canvas></div>
    </section>

    <div class="tableWrap">
      <table>
        <thead>
          <tr>
            <th>Frequency</th>
            <th>Real</th>
            <th>Imag</th>
            <th>|Z| Ohm</th>
            <th>Phase Deg</th>
            <th>Resistance R Ohm</th>
            <th>Reactance X Ohm</th>
          </tr>
        </thead>
        <tbody id="dataRows"></tbody>
      </table>
    </div>
  </main>

  <script>
    let zChart;
    let phaseChart;
    let reactanceChart;
    let resistanceReactanceChart;

    function fmt(value, digits = 3) {
      if (value === null || value === undefined || Number.isNaN(Number(value))) return '';
      return Number(value).toFixed(digits);
    }

    async function runAction(path, message) {
      const status = document.getElementById('status');
      status.textContent = message;
      try {
        const response = await fetch(path);
        const result = await response.json();
        status.textContent = result.message || (result.ok ? 'Done' : 'Failed');
        await loadData();
        await loadSessions();
      } catch (error) {
        status.textContent = 'Request failed: ' + error.message;
      }
    }

    async function loadData() {
      const response = await fetch('/data');
      const payload = await response.json();
      const rows = payload.readings || [];
      document.getElementById('status').textContent = payload.message || ('Loaded ' + rows.length + ' readings');

      renderRows(rows);
      renderCharts(rows);
    }

    async function refreshDashboard() {
      await loadData();
      await loadSessions();
    }

    async function loadSessions() {
      const response = await fetch('/sessions');
      const payload = await response.json();
      const sessions = payload.sessions || [];
      const list = document.getElementById('sessionList');

      if (sessions.length === 0) {
        list.innerHTML = '<div class="sessionItem"><span class="sessionName">No saved sessions</span></div>';
        return;
      }

      list.innerHTML = sessions.map(session => {
        const file = encodeURIComponent(session.file);
        return '<div class="sessionItem">' +
          '<span class="sessionName">' + session.file + ' (' + session.size + ' bytes)</span>' +
          '<button class="secondary" onclick="viewSession(\'' + file + '\')">Open</button>' +
          '<a class="button secondary" href="/download?file=' + file + '">Download CSV</a>' +
          '<button onclick="deleteSession(\'' + file + '\')">Delete</button>' +
          '</div>';
      }).join('');
    }

    async function viewSession(encodedFile) {
      const response = await fetch('/session?file=' + encodedFile);
      const payload = await response.json();
      const rows = payload.readings || [];
      document.getElementById('status').textContent = 'Viewing ' + payload.file;
      renderRows(rows);
      renderCharts(rows);
    }

    async function deleteSession(encodedFile) {
      if (!confirm('Delete this saved session?')) return;
      const response = await fetch('/delete?file=' + encodedFile);
      const result = await response.json();
      document.getElementById('status').textContent = result.message || 'Delete complete';
      await loadSessions();
    }

    async function uploadSavedCsv(event) {
      const input = event.target;
      const file = input.files && input.files[0];
      if (!file) return;

      try {
        const text = await file.text();
        const rows = parseCsvReadings(text);
        if (rows.length === 0) {
          document.getElementById('status').textContent = 'No valid readings found in ' + file.name;
          return;
        }

        renderRows(rows);
        renderCharts(rows);
        document.getElementById('status').textContent = 'Loaded ' + rows.length + ' readings from ' + file.name;
      } catch (error) {
        document.getElementById('status').textContent = 'CSV upload failed: ' + error.message;
      } finally {
        input.value = '';
      }
    }

    function parseCsvReadings(text) {
      const lines = text.split(/\r?\n/).map(line => line.trim()).filter(line => line.length > 0);
      if (lines.length < 2) return [];

      return lines.slice(1).map(line => {
        const parts = line.split(',').map(value => value.trim());
        if (parts.length < 7) return null;

        const row = {
          frequency: Number(parts[0]),
          real: Number(parts[1]),
          imag: Number(parts[2]),
          zMagnitude: Number(parts[3]),
          phaseDeg: Number(parts[4]),
          resistance: Number(parts[5]),
          reactance: Number(parts[6])
        };

        return Object.values(row).every(value => Number.isFinite(value)) ? row : null;
      }).filter(row => row !== null);
    }

    function renderRows(rows) {
      const tbody = document.getElementById('dataRows');
      tbody.innerHTML = rows.map(row =>
        '<tr>' +
        '<td>' + fmt(row.frequency, 0) + '</td>' +
        '<td>' + row.real + '</td>' +
        '<td>' + row.imag + '</td>' +
        '<td>' + fmt(row.zMagnitude, 3) + '</td>' +
        '<td>' + fmt(row.phaseDeg, 3) + '</td>' +
        '<td>' + fmt(row.resistance, 3) + '</td>' +
        '<td>' + fmt(row.reactance, 3) + '</td>' +
        '</tr>'
      ).join('');
    }

    function renderCharts(rows) {
      const labels = rows.map(row => row.frequency);
      const zValues = rows.map(row => row.zMagnitude);
      const phaseValues = rows.map(row => row.phaseDeg);
      const xValues = rows.map(row => Math.abs(row.reactance));
      const resistanceReactanceValues = rows.map(row => ({
        x: row.resistance,
        y: row.reactance
      }));

      if (zChart) zChart.destroy();
      if (phaseChart) phaseChart.destroy();
      if (reactanceChart) reactanceChart.destroy();
      if (resistanceReactanceChart) resistanceReactanceChart.destroy();

      zChart = new Chart(document.getElementById('zChart'), {
        type: 'line',
        data: {
          labels,
          datasets: [{
            label: '|Z| Ohm',
            data: zValues,
            borderColor: '#244a8f',
            backgroundColor: 'rgba(36, 74, 143, 0.12)',
            pointRadius: 2,
            tension: 0.18
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          plugins: {
            title: { display: true, text: '|Z| vs Frequency' }
          },
          scales: {
            x: { title: { display: true, text: 'Frequency (Hz)' } },
            y: { title: { display: true, text: '|Z| (Ohm)' } }
          }
        }
      });

      phaseChart = new Chart(document.getElementById('phaseChart'), {
        type: 'line',
        data: {
          labels,
          datasets: [{
            label: 'Phase Deg',
            data: phaseValues,
            borderColor: '#7a2e83',
            backgroundColor: 'rgba(122, 46, 131, 0.10)',
            pointRadius: 2,
            tension: 0.18
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          plugins: {
            title: { display: true, text: 'Phase vs Frequency' }
          },
          scales: {
            x: { title: { display: true, text: 'Frequency (Hz)' } },
            y: { title: { display: true, text: 'Phase (Deg)' } }
          }
        }
      });

      reactanceChart = new Chart(document.getElementById('reactanceChart'), {
        type: 'line',
        data: {
          labels,
          datasets: [{
            label: '|Reactance X| Ohm',
            data: xValues,
            borderColor: '#b42318',
            backgroundColor: 'rgba(180, 35, 24, 0.10)',
            pointRadius: 2,
            tension: 0.18
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          plugins: {
            title: { display: true, text: '|Reactance X| vs Frequency' }
          },
          scales: {
            x: { title: { display: true, text: 'Frequency (Hz)' } },
            y: { title: { display: true, text: '|Reactance X| (Ohm)' } }
          }
        }
      });

      resistanceReactanceChart = new Chart(document.getElementById('resistanceReactanceChart'), {
        type: 'scatter',
        data: {
          datasets: [{
            label: 'Reactance X vs Resistance R',
            data: resistanceReactanceValues,
            borderColor: '#177245',
            backgroundColor: 'rgba(23, 114, 69, 0.14)',
            pointRadius: 3,
            showLine: true,
            tension: 0.12
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          plugins: {
            title: { display: true, text: 'Reactance X vs Resistance R' }
          },
          scales: {
            x: { title: { display: true, text: 'Resistance R (Ohm)' } },
            y: { title: { display: true, text: 'Reactance X (Ohm)' } }
          }
        }
      });

    }

    loadData();
    loadSessions();
  </script>
</body>
</html>
)rawliteral";

#endif
