set -eu
mac=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -m) [ "$#" -gt 1 ] || { printf '%s\n' 'Usage: wol.sh -m MAC' >&2; exit 2; }; mac=$2; shift 2 ;;
        *) printf '%s\n' 'Usage: wol.sh -m MAC' >&2; exit 2 ;;
    esac
done
[ -n "$mac" ] || { printf '%s\n' 'Usage: wol.sh -m MAC' >&2; exit 2; }
apt install -y python
python - "$mac" <<'PY'
import ipaddress
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
address = sys.argv[1]
value = address.replace(":", "").replace("-", "")
if len(value) != 12 or any(char not in "0123456789abcdefABCDEF" for char in value):
    raise SystemExit("invalid MAC address")
packet = b"\xff" * 6 + bytes.fromhex(value) * 16
def wake():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        for _ in range(3):
            sock.sendto(packet, ("255.255.255.255", 9))
tailnet = ipaddress.ip_network("100.64.0.0/10")
class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/wake" or ipaddress.ip_address(self.client_address[0]) not in tailnet:
            self.send_error(403)
            return
        wake()
        self.send_response(204)
        self.end_headers()
    def log_message(self, format, *args):
        pass
ThreadingHTTPServer(("0.0.0.0", 8765), Handler).serve_forever()
PY