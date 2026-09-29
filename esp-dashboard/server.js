const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 3000;
let clients = [];
let latestData = {
  pressure_bar: 200.0,
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

  // SSE Stream for Browser Dashboard Live Updates
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

  // REST API: ESP32 posts data to /data
  if (req.method === 'POST' && req.url === '/data') {
    let body = '';
    req.on('data', chunk => { body += chunk.toString(); });
    req.on('end', () => {
      try {
        const parsed = JSON.parse(body);
        latestData = { ...parsed, timestamp: new Date().toLocaleTimeString() };
        console.log(`[${new Date().toLocaleTimeString()}] ESP32 Data Received:`, latestData);

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

  // Serve Dashboard HTML
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

      res.writeHead(200, { 'Content-Type': contentType });
      res.end(content);
    }
  });
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`====================================================`);
  console.log(`🚀 ESP32 Dashboard Server Running on Port ${PORT}`);
  console.log(`👉 Local Dashboard:   http://localhost:${PORT}`);
  console.log(`👉 ESP32 POST Target: http://10.165.255.7:${PORT}/data`);
  console.log(`====================================================`);
});
