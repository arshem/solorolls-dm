FROM python:3.12-slim

WORKDIR /app

COPY local/requirements.txt /app/local/
RUN pip install --no-cache-dir -r /app/local/requirements.txt

COPY local/server.py local/models.py /app/local/
COPY worker/public /app/worker/public

EXPOSE 8787

CMD ["python", "/app/local/server.py"]
