FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY adxo_service.py .
COPY templates/ templates/

EXPOSE 8083

CMD ["python", "adxo_service.py"]
