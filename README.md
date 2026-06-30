# Real-Time Multi-Client Chat (C / POSIX)

## Build

```bash
gcc -o server server.c -lpthread
gcc -o client client.c -lpthread
```

## Run

```bash
# Terminal 1 – start server on port 9090
./server 9090

# Terminal 2+ – connect clients (use 127.0.0.1 for local, or LAN IP)
./client 127.0.0.1 9090
```

## Auth Flow

On connect you choose **Login** or **Register**.  
Credentials are stored in `users.txt` (plaintext – swap for hashed in production).

## Commands

| Command | Description |
|---|---|
| `/help` | Show command list |
| `/online` | List connected users |
| `/msg <user> <text>` | Private message |
| `/exit` | Disconnect |

## Files

| File | Purpose |
|---|---|
| `server.c` | Multi-threaded server |
| `client.c` | Interactive client |
| `users.txt` | Credential store |
| `chat_history.txt` | Persistent message log |

## LAN / Cross-Network

Bind the server to `0.0.0.0` (already the default via `INADDR_ANY`).  
Clients connect using the server machine's LAN IP (e.g. `192.168.1.x`).  
Open the chosen port in your firewall if needed.
