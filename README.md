# NGINX Distributed Cluster Deployment

This folder contains everything needed to deploy the 185k+ TPS Unary architecture across 3 separate laptops. This eliminates SSL/JWT overhead from the database by offloading it to NGINX.

## Laptop 1 (The Backend Database)
**Role:** Runs the `proxy_backend_server`. It has no SSL, no JWT, and runs at maximum physical speed using the Nitro+ RocksDB profile.
**Setup:**
1. Navigate to `Nginx/laptop1_backend`
2. Run `mkdir build && cd build`
3. Run `cmake .. && make -j$(nproc) proxy_backend_server`
4. Start the server: `./proxy_backend_server`
*(Note: It listens on `0.0.0.0:50051`-`50058` insecurely).*

## Laptop 2 (The NGINX Proxy)
**Role:** Acts as the shield. It receives secure SSL traffic from the internet/network, decrypts it using the provided certificates, and forwards the raw bytes to Laptop 1.
**Setup:**
1. Install NGINX (`sudo apt install nginx`)
2. Navigate to `Nginx/laptop2_proxy`
3. Edit `nginx.conf`: Change the `Server 127.0.0.1` lines to the actual Wi-Fi/LAN IP of **Laptop 1**.
4. Edit `nginx.conf`: Ensure the `ssl_certificate` paths point to the `server.crt` and `server.key` in this folder.
5. Override the default NGINX config: `sudo cp nginx.conf /etc/nginx/nginx.conf`
6. Restart NGINX: `sudo systemctl restart nginx`

## Laptop 3 (The Client)
**Role:** Simulates thousands of mobile phones. Connects securely to the NGINX proxy using SSL and JWT authentication.
**Setup:**
1. Navigate to `Nginx/laptop3_client`
2. Edit `load_client.cpp` (Lines 178 & 187): Change `"localhost"` to the IP address of **Laptop 2 (NGINX)**.
3. Run `mkdir build && cd build`
4. Run `cmake .. && make kv_client`
5. Run the high-speed benchmark: `./kv_client 12 200 10`
