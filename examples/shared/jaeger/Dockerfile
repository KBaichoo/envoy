FROM jaegertracing/all-in-one@sha256:7885400a153ac908d8bfbf72c27e3389dbab1942a35dd0ce3228611dc48cbf9f
HEALTHCHECK \
    --interval=1s \
    --timeout=1s \
    --start-period=1s \
    --retries=60 \
    CMD wget -q --header='Content-Type:application/json' -O - http://localhost:14269/health | grep "Server available"
