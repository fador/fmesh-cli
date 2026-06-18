#!/usr/bin/env python3
import socket
import ssl
import time
import threading
import sys
import random

HOST = '127.0.0.1'
PORT = 4403

def test_slowloris():
    print("[*] Running Slowloris test...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    print("[*] Slowloris connected, sleeping for 10 seconds...")
    time.sleep(10)
    try:
        s.sendall(b"garbage")
    except Exception as e:
        print(f"[*] Slowloris disconnected as expected: {e}")
    finally:
        s.close()

def test_garbage_handshake():
    print("[*] Running Garbage Handshake test...")
    for _ in range(10):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        garbage = bytes([random.randint(0, 255) for _ in range(1024)])
        try:
            s.sendall(garbage)
        except Exception:
            pass
        finally:
            s.close()
    print("[*] Garbage Handshake finished.")

def test_auth_brute_force():
    print("[*] Running Auth Brute-Force test (with TLS)...")
    context = ssl._create_unverified_context()
    for i in range(5):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.connect((HOST, PORT))
            ssock = context.wrap_socket(s, server_hostname=HOST)
            start = time.time()
            ssock.sendall(b"AUTH admin wrongpass\n")
            data = ssock.recv(10)
            elapsed = time.time() - start
            print(f"[*] Auth attempt {i}: received {data}, took {elapsed:.2f}s")
            ssock.close()
        except Exception as e:
            print(f"[*] Auth brute-force error: {e}")

def hold_connection(id):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect((HOST, PORT))
        time.sleep(5)
    except Exception:
        pass
    finally:
        s.close()

def test_max_connections():
    print("[*] Running Max Connections test (spawning 100 threads)...")
    threads = []
    for i in range(100):
        t = threading.Thread(target=hold_connection, args=(i,))
        t.start()
        threads.append(t)
    
    for t in threads:
        t.join()
    print("[*] Max Connections test finished.")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        PORT = int(sys.argv[1])
    
    print(f"Starting Fuzz Tests against {HOST}:{PORT}")
    
    test_max_connections()
    test_slowloris()
    test_garbage_handshake()
    test_auth_brute_force()
    
    print("[*] All fuzz tests finished. The server should still be running and responsive.")
