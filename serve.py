#!/usr/bin/env python3
"""Generate a QR code for a .cia URL and serve it over HTTP.

Usage: python serve.py <build_dir> [port]
  e.g. python serve.py mgba/build6 8084
"""
import sys, os, socket, http.server, socketserver

if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} <build_dir> [port]")
    sys.exit(1)

build_dir = os.path.join(os.path.abspath(sys.argv[1]), "3ds")
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

if not os.path.isdir(build_dir):
    print(f"Error: {build_dir} not found")
    sys.exit(1)

# Get local IP
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.connect(("8.8.8.8", 80))
ip = s.getsockname()[0]
s.close()

url = f"http://{ip}:{port}/mgba.cia"

# Generate QR
try:
    import qrcode
    qr = qrcode.make(url)
    qr.save(os.path.join(build_dir, "qr.png"))
    print(f"QR saved to {build_dir}/qr.png")
except ImportError:
    print("qrcode module not installed, skipping QR generation")

print(f"URL: {url}")
os.chdir(build_dir)
handler = http.server.SimpleHTTPRequestHandler
httpd = socketserver.TCPServer(("0.0.0.0", port), handler)
print(f"Serving {build_dir} on port {port}...")
httpd.serve_forever()