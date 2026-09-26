FROM ghcr.io/xinnan-tech/xiaozhi-esp32-server@sha256:4673fac2cb7125d6370a3e8cd6cd9985195c9bfc6203d3ced41e86a1b9775c30

LABEL org.opencontainers.image.source="https://github.com/xinnan-tech/xiaozhi-esp32-server"
LABEL org.opencontainers.image.revision="6afc54a17def47578a4b3efc4680873689d3168b"
LABEL org.opencontainers.image.description="ESP Media authenticated conversation-reset and device-audio overlay"

USER root
COPY . /opt/esp-media-server-overlay
RUN python /opt/esp-media-server-overlay/apply_overlay.py /opt/xiaozhi-esp32-server \
    && cd /opt/xiaozhi-esp32-server \
    && python -m unittest discover -s /opt/esp-media-server-overlay/tests -v \
    && install -d /opt/xiaozhi-esp32-server/licenses \
    && cp /opt/esp-media-server-overlay/XIAOZHI_MIT_LICENSE.txt \
          /opt/xiaozhi-esp32-server/licenses/xiaozhi-esp32-server-MIT.txt \
    && cp /opt/esp-media-server-overlay/reset_integration.py \
          /opt/xiaozhi-esp32-server/reset_integration.py \
    && rm -rf /opt/esp-media-server-overlay
