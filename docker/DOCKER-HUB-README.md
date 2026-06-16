# Transmission Seeder

Custom `transmission-daemon` docker image optimized for high-performance seeding, based on Transmission **4.1.2** with an adaptive **Quick-Verify** algorithm.

* **Source Code**: [GitHub - std2main/transmission](https://github.com/std2main/transmission)
* **Docker Hub**: [std2main/transmission-seeder](https://hub.docker.com/r/std2main/transmission-seeder)
* **Supported Platforms**: `linux/amd64`, `linux/arm64`, `linux/arm/v7`, `linux/386`

---

## Key Feature: Quick-Verify

Standard Transmission reads every byte of a torrent file from disk to verify integrity upon startup or recheck, causing significant disk I/O bottlenecks for large seeding setups.

This image features an adaptive **Quick-Verify** implementation that performs deterministic piece sampling (using a mathematical interval based on torrent size) to verify integrity. This reduces disk read operations by up to **90%** while maintaining reliable integrity checks.

---

## Quick Start (Docker Compose)

The easiest way to run the daemon is using `docker-compose`. Create a `compose.yaml` file:

```yaml
services:
  transmission:
    image: std2main/transmission-seeder:latest
    container_name: transmission-seeder
    restart: unless-stopped
    environment:
      # Enable quick-verify by default
      TRANSMISSION_QUICK_VERIFY_ENABLED: "true"
      # If quick-verify finds a mismatch, do NOT automatically fall back to full check
      TRANSMISSION_QUICK_VERIFY_FALLBACK_ENABLED: "false"
      
      # Standard Transmission Settings
      TRANSMISSION_RPC_BIND_ADDRESS: 0.0.0.0
      TRANSMISSION_RPC_PORT: "9091"
      TRANSMISSION_RPC_WHITELIST_ENABLED: "false"
      TRANSMISSION_PEER_PORT: "51413"
    ports:
      - "9091:9091" # RPC / Web UI
      - "51413:51413/tcp"
      - "51413:51413/udp"
    volumes:
      - ./config:/config
      - ./downloads:/downloads
      - ./watch:/watch
```

Run the container:
```bash
docker compose up -d
```
Access Web UI at `http://localhost:9091`.

---

## Environment Configuration Variables

You can configure both the custom quick-verify switches and standard settings via environment variables:

| Variable | Default | Description |
| :--- | :---: | :--- |
| `TRANSMISSION_QUICK_VERIFY_ENABLED` | `false` | Enables the sampled quick-verification logic. |
| `TRANSMISSION_QUICK_VERIFY_FALLBACK_ENABLED` | `false` | If enabled, a mismatch in quick-verify triggers a full, block-by-block verification of the entire torrent. |
| `TRANSMISSION_RPC_PORT` | `9091` | Port for the RPC server and Web UI. |
| `TRANSMISSION_RPC_WHITELIST_ENABLED` | `false` | Enable/Disable Web UI access whitelist IP verification. |
| `TRANSMISSION_PEER_PORT` | `51413` | Port for incoming peer connections. |
| `TRANSMISSION_DOWNLOAD_DIR` | `/downloads` | Default download folder path. |
| `TRANSMISSION_INCOMPLETE_DIR_ENABLED`| `false` | Enable/Disable writing partial files to a separate folder. |

---

## Quick-Verify Scenarios

1. **Strict Seeding (Recommended for Seeding boxes)**:
   * `TRANSMISSION_QUICK_VERIFY_ENABLED=true`
   * `TRANSMISSION_QUICK_VERIFY_FALLBACK_ENABLED=false`
   * **Behavior**: Uses quick verification. If any sampled piece fails verification, verification immediately fails and blocks seeding. No heavy disk reads are executed.
2. **Auto-Recovery**:
   * `TRANSMISSION_QUICK_VERIFY_ENABLED=true`
   * `TRANSMISSION_QUICK_VERIFY_FALLBACK_ENABLED=true`
   * **Behavior**: Uses quick verification. If a mismatch is detected, it automatically initiates a full block-by-block recheck of the entire file to locate and download only the broken parts.
3. **Standard Transmission**:
   * `TRANSMISSION_QUICK_VERIFY_ENABLED=false`
   * **Behavior**: Disables quick-verify entirely; acts like default Transmission (reads and verifies every single block on disk).

---

## ⚠️ Warning: Risk of Undetected Corruption (危险提示)

Since **Quick-Verify** only samples a subset of pieces from the torrent payload rather than hashing every block on disk, there is a **mathematical possibility of undetected data corruption** if corruption occurs exclusively in a non-sampled block. 

* **When to use**: Highly recommended for large, static seeding setups (e.g. private tracker seeding) where files are already written to stable storage and not modified.
* **When NOT to use**: Do not use this switch on volatile drives, unstable hardware (flaky cables, bad RAM), or in setups where silent bitrot is highly suspected. If absolute 100% data integrity guarantees are required, disable quick-verify.

---

## 📋 Considerations & Best Practices (注意事项)

1. **Auto-Recovery Switch**: If you are using this on public trackers or have unreliable internet/storage, it is recommended to set `TRANSMISSION_QUICK_VERIFY_FALLBACK_ENABLED=true` so that any detected mismatch triggers a full verify to automatically repair the torrent.
2. **Third-party Apps**: This modification is built into the daemon and handles torrent verification internally. Web UI toggles and RPC API endpoints are extended but completely backward-compatible.
3. **Upgrade/Downgrade safety**: The config file adjustments are backward-compatible. You can safely downgrade back to standard Transmission at any time by simply switching images.

---

## ⚖️ Disclaimer (免责声明)

* **No Warranty**: This software is provided "as is", without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose and noninfringement.
* **Not Affiliated**: This custom build is independent and is **NOT** officially affiliated with or endorsed by the official Transmission project. Do not report bugs related to the Quick-Verify features to the upstream Transmission repository.
