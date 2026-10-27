# Sparkplug B host: Mosquitto -> QuestDB.
# Build context is the REPO ROOT, so the same proto/sparkplug_b.proto that
# the firmware uses is compiled into the Python bindings here:
#   docker build -f "Cloud Server/sparkplug-host/Dockerfile" .
FROM python:3.13-slim

WORKDIR /app

COPY ["Cloud Server/sparkplug-host/requirements.txt", "./"]
RUN pip install --no-cache-dir -r requirements.txt grpcio-tools==1.85.0

COPY proto/sparkplug_b.proto ./
RUN python -m grpc_tools.protoc -I. --python_out=. sparkplug_b.proto \
 && pip uninstall -y grpcio-tools grpcio

COPY ["Cloud Server/sparkplug-host/host.py", "Cloud Server/sparkplug-host/fake_edge_node.py", "./"]

# Don't run as root inside the container
RUN useradd --system --uid 10001 sparkplug
USER sparkplug

CMD ["python", "-u", "host.py"]
