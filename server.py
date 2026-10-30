import sys
import time
import threading
import logging
from flask import Flask, request, jsonify, Response

log = logging.getLogger('werkzeug')
log.setLevel(logging.ERROR)

app = Flask(__name__)

latest_frame = None
last_client_request_time = 0
last_esp32_post_time = 0

CLIENT_TIMEOUT = 30  # 30秒アクセスなしで視聴停止
ESP32_TIMEOUT = 25   # 25秒送信なしでオフライン

@app.route('/upload', methods=['POST'])
def upload():
    global latest_frame, last_esp32_post_time
    latest_frame = request.data
    last_esp32_post_time = time.time()
    
    # 到着を即時SSH画面へ出力
    sys.stdout.write(f"\n[SSH Log] ● RECEIVED FRAME ({len(latest_frame)} bytes at {time.strftime('%H:%M:%S')})\r")
    sys.stdout.flush()
    
    is_client_active = (time.time() - last_client_request_time) < CLIENT_TIMEOUT
    return jsonify({"stream": is_client_active})

@app.route('/status', methods=['GET'])
def status():
    global last_client_request_time
    last_client_request_time = time.time()
    return jsonify({"stream": True})

@app.route('/latest.jpg')
def latest_jpg():
    global last_client_request_time
    last_client_request_time = time.time()
    if latest_frame:
        return Response(latest_frame, mimetype='image/jpeg')
    else:
        return "No frame yet", 404

@app.route('/')
def index():
    return """
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="utf-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>IoT SIM Camera</title>
        <style>
            body { background-color: #121212; color: #fff; font-family: sans-serif; text-align: center; padding: 20px; }
            .container { max-width: 640px; margin: 0 auto; }
            #cam-img { max-width: 100%; height: auto; border: 1px solid #444; border-radius: 8px; background: #000; }
            .info { color: #888; font-size: 13px; margin-top: 10px; }
            .status { color: #4caf50; font-weight: bold; margin-bottom: 12px; }
        </style>
    </head>
    <body>
        <div class="container">
            <h2>IoT SIM Camera (10秒自動更新)</h2>
            <div class="status">10秒間隔で更新中...</div>
            <img id="cam-img" src="/latest.jpg" alt="画像を読み込み中..." onerror="this.style.display='none';">
            <div class="info">最終更新: <span id="last-update">-</span></div>
        </div>
        <script>
            function refreshImage() {
                const img = document.getElementById('cam-img');
                const updateText = document.getElementById('last-update');
                const newSrc = "/latest.jpg?t=" + new Date().getTime();
                const tempImg = new Image();
                tempImg.onload = function() {
                    img.src = newSrc;
                    img.style.display = 'inline-block';
                    updateText.innerText = new Date().toLocaleTimeString();
                };
                tempImg.src = newSrc;
            }
            refreshImage();
            setInterval(refreshImage, 10000);
        </script>
    </body>
    </html>
    """

def console_status_monitor():
    dots = [".  ", ".. ", "...", "   "]
    idx = 0

    print("\n=== Relay Server Started (10s Polling Mode) ===")

    while True:
        is_esp32_online = (time.time() - last_esp32_post_time) < ESP32_TIMEOUT
        if not is_esp32_online:
            dot_str = dots[idx % len(dots)]
            sys.stdout.write(f"\r[SSH Log] Waiting for ESP32 frame{dot_str}")
            sys.stdout.flush()
            idx += 1
        time.sleep(1)

if __name__ == '__main__':
    monitor_thread = threading.Thread(target=console_status_monitor, daemon=True)
    monitor_thread.start()
    app.run(host='0.0.0.0', port=5000)