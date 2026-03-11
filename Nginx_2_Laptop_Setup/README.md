# NGINX 2-Laptop Setup (Ultimate Throughput)

This folder contains the highly optimized 2-Laptop configuration designed to bypass the Wi-Fi "4-Hop Nightmare". 

By moving NGINX to the exact same physical laptop as the RocksDB backend, we cut the wireless hops in half and completely eliminate network packet collisions between the Proxy and the Database.

## Laptop 1: The i5 Server and Proxy (`192.168.0.130`)
**Role:** Runs *both* NGINX (to decrypt SSL) and proxy_backend_server (to handle the database writes).

**Step 1: Start the Backend Database**
1. Navigate to `Nginx_2_Laptop_Setup/laptop1_i5_server_and_proxy/`
2. Run `mkdir build && cd build`
3. Run `cmake .. && make -j$(nproc) proxy_backend_server`
4. Start the server: `./proxy_backend_server`
*(It will listen securely on localhost ports 50061-50068).*

**Step 2: Start the NGINX Proxy**
1. Install NGINX (`sudo apt install nginx`)
2. In another terminal, navigate to `Nginx_2_Laptop_Setup/laptop1_i5_server_and_proxy/`
3. Copy the optimized config: `sudo cp nginx.conf /etc/nginx/nginx.conf`
4. Copy certificates: `sudo cp server.crt /etc/nginx/server.crt` and `sudo cp server.key /etc/nginx/server.key`
5. Restart NGINX: `sudo systemctl restart nginx`

## Laptop 2: The i3 Client (`192.168.0.178`)
**Role:** Simulates the massive mobile traffic. Connects securely to the NGINX proxy on the i5 laptop via Wi-Fi/LAN.

**Setup:**
1. Navigate to `Nginx_2_Laptop_Setup/laptop2_i3_client/`
2. Run `mkdir build && cd build`
3. Run `cmake .. && make kv_client`
4. Run the high-speed benchmark: `./kv_client 12 200 10`

*(The code is already hardcoded to point to the i5's IP at `192.168.0.130`).*
