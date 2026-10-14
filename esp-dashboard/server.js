const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = process.env.PORT || 3000;
let clients = [];

// Default telemetry state
let latestData = {
  pressure_bar: 200.0,
  pressure_kpa: 40.0,
  oxygen_percent: 100.0,
  remaining_liters: 2000,
  flow_rate_lmin: 0.0,
  time_remaining_mins: 400,
  time_to_target_20_mins: 320,
  status: 'NORMAL',
  timestamp: new Date().toLocaleTimeString()
};

const server = http.createServer((req, res) => {
  // CORS Headers
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  // REST API: GET current telemetry (Supported on Vercel Serverless & Local)
  if (req.method === 'GET' && (req.url === '/data' || req.url === '/api/data')) {
    res.writeHead(200, { 
      'Content-Type': 'application/json',
      'Cache-Control': 'no-cache, no-store, must-revalidate'
    });
    res.end(JSON.stringify(latestData));
    return;
  }

  // SSE Stream for Browser Dashboard Live Updates (Local persistent server)
  if (req.method === 'GET' && req.url === '/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive'
    });
    res.write(`data: ${JSON.stringify(latestData)}\n\n`);
    clients.push(res);

    req.on('close', () => {
      clients = clients.filter(c => c !== res);
    });
    return;
  }

  // REST API: ESP32 posts live telemetry to /data
  if (req.method === 'POST' && (req.url === '/data' || req.url === '/api/data')) {
    let body = '';
    req.on('data', chunk => { body += chunk.toString(); });
    req.on('end', () => {
      try {
        const parsed = JSON.parse(body);

        // Calculate missing derived fields if needed
        let bar = (parsed.pressure_bar !== undefined) ? parseFloat(parsed.pressure_bar) : latestData.pressure_bar;
        let kpa = (parsed.pressure_kpa !== undefined) ? parseFloat(parsed.pressure_kpa) : (bar * 100.0);
        let percent = (parsed.oxygen_percent !== undefined) ? parseFloat(parsed.oxygen_percent) : 
                      ((parsed.percent !== undefined) ? parseFloat(parsed.percent) : (bar / 200.0 * 100.0));
        let liters = (parsed.remaining_liters !== undefined) ? parseFloat(parsed.remaining_liters) : (bar * 10.0);
        let flow = (parsed.flow_rate_lmin !== undefined) ? parseFloat(parsed.flow_rate_lmin) : 0.0;
        let timeRemaining = (parsed.time_remaining_mins !== undefined) ? parseFloat(parsed.time_remaining_mins) : 
                            (flow > 0.1 ? (liters / flow) : (liters / 5.0));

        latestData = {
          ...parsed,
          pressure_bar: bar,
          pressure_kpa: kpa,
          oxygen_percent: Math.min(Math.max(percent, 0), 100),
          remaining_liters: Math.max(liters, 0),
          flow_rate_lmin: flow,
          time_remaining_mins: Math.max(timeRemaining, 0),
          status: parsed.status || (percent <= 15 ? 'CRITICAL' : percent <= 30 ? 'LOW' : 'NORMAL'),
          timestamp: new Date().toLocaleTimeString()
        };

        console.log(`[${new Date().toLocaleTimeString()}] ESP32 Telemetry -> Pressure: ${bar.toFixed(3)} BAR | O2: ${latestData.oxygen_percent.toFixed(1)}% | Vol: ${latestData.remaining_liters.toFixed(0)}L`);

        // Broadcast to all connected web dashboard clients
        clients.forEach(client => {
          client.write(`data: ${JSON.stringify(latestData)}\n\n`);
        });

        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'ok', received: latestData }));
      } catch (err) {
        console.error('Invalid JSON received:', body);
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'Invalid JSON' }));
      }
    });
    return;
  }

  // Serve Dashboard HTML and Static Assets
  let filePath = path.join(__dirname, 'public', req.url === '/' ? 'index.html' : req.url);
  fs.readFile(filePath, (err, content) => {
    if (err) {
      if (err.code === 'ENOENT') {
        fs.readFile(path.join(__dirname, 'public', 'index.html'), (err2, htmlContent) => {
          if (err2) {
            res.writeHead(404);
            res.end('404 Not Found');
          } else {
            res.writeHead(200, { 'Content-Type': 'text/html' });
            res.end(htmlContent);
          }
        });
      } else {
        res.writeHead(500);
        res.end(`Server Error: ${err.code}`);
      }
    } else {
      const ext = path.extname(filePath);
      let contentType = 'text/html';
      if (ext === '.js') contentType = 'text/javascript';
      if (ext === '.css') contentType = 'text/css';
      if (ext === '.json') contentType = 'application/json';
      if (ext === '.svg') contentType = 'image/svg+xml';

      res.writeHead(200, { 'Content-Type': contentType });
      res.end(content);
    }
  });
});

if (require.main === module) {
  server.listen(PORT, '0.0.0.0', () => {
    console.log(`====================================================`);
    console.log(`🚀 ESP32 Oxygen Cylinder Dashboard Running on Port ${PORT}`);
    console.log(`👉 Local Web Dashboard: http://localhost:${PORT}`);
    console.log(`👉 ESP32 Ingestion URL: http://10.20.235.7:${PORT}/data`);
    console.log(`====================================================`);
  });
}

module.exports = server;
