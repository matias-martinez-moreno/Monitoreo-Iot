"""
auth_service.py - Servicio Externo de Autenticación
Lenguaje: Python + Flask
Puerto: 9000

Servicio independiente del servidor principal.
El servidor C consulta este servicio via HTTP para validar credenciales.

Uso: python auth_service.py [--port 9000] [--host 0.0.0.0]
"""

import argparse
import hashlib
import json
import logging
from flask import Flask, request, jsonify

app = Flask(__name__)

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s - %(message)s"
)

# Base de usuarios (en producción usar base de datos real)
# Contraseñas almacenadas como SHA-256
USERS = {
    "admin":    hashlib.sha256(b"admin123").hexdigest(),
    "operador": hashlib.sha256(b"oper2024").hexdigest(),
    "ingeniero": hashlib.sha256(b"ing2024").hexdigest(),
}


def hash_password(password: str) -> str:
    return hashlib.sha256(password.encode()).hexdigest()


@app.route("/auth", methods=["GET"])
def authenticate():
    """
    Endpoint de autenticación.
    Query params: user=<usuario>&pass=<contraseña>

    Respuestas:
        200 OK  + {"status": "ok", "user": "..."}
        401     + {"status": "fail", "reason": "..."}
    """
    username = request.args.get("user", "").strip()
    password = request.args.get("pass", "").strip()

    client_ip = request.remote_addr
    logging.info(f"Intento de login - user={username} desde {client_ip}")

    if not username or not password:
        logging.warning(f"Credenciales vacías desde {client_ip}")
        return jsonify({"status": "fail", "reason": "Credenciales vacías"}), 401

    stored_hash = USERS.get(username)
    if stored_hash and stored_hash == hash_password(password):
        logging.info(f"Login exitoso para usuario: {username}")
        return jsonify({"status": "ok", "user": username}), 200
    else:
        logging.warning(f"Login fallido para usuario: {username}")
        return jsonify({"status": "fail", "reason": "Usuario o contraseña incorrectos"}), 401


@app.route("/health", methods=["GET"])
def health():
    """Endpoint de verificación de estado del servicio."""
    return jsonify({"status": "healthy", "service": "auth"}), 200


@app.route("/users", methods=["GET"])
def list_users():
    """Lista de usuarios disponibles (sin contraseñas)."""
    return jsonify({"users": list(USERS.keys())}), 200


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Servicio de Autenticación IoT")
    parser.add_argument("--host", default="0.0.0.0", help="Host a escuchar")
    parser.add_argument("--port", type=int, default=9000, help="Puerto")
    args = parser.parse_args()

    logging.info(f"Servicio de autenticacion iniciado en {args.host}:{args.port}")
    logging.info(f"Usuarios registrados: {list(USERS.keys())}")
    app.run(host=args.host, port=args.port, debug=False)
