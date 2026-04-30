import socket
import time

# 서버 연결
server_ip = "127.0.0.1"
server_port = 5001

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect((server_ip, server_port))

print(f"[클라이언트] 서버 {server_ip}:{server_port}에 연결됨\n")

# 테스트 데이터 전송
test_cases = [
    (1, 37.5, 127.0),      # 사용자 1, 위도 37.5, 경도 127.0
    (2, 37.501, 127.001),  # 사용자 2, 조금 다른 위치
    (3, 37.5, 127.0),      # 사용자 3, 같은 위치
    (4, 37.51, 127.01),    # 사용자 4, 다른 zone
]

for user_id, lat, lng in test_cases:
    message = f"{user_id},{lat},{lng}"
    sock.send(message.encode())
    
    print(f"[전송] {message}")
    
    # 응답 받기
    response = sock.recv(1024).decode()
    print(f"[응답] {response}\n")
    
    time.sleep(0.5)

sock.close()
print("[클라이언트] 연결 종료")