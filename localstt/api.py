from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Annotated

from fastapi import FastAPI, File, Form, UploadFile
from fastapi.responses import JSONResponse, PlainTextResponse

from .service import STTService, result_to_json


def create_app(service: STTService) -> FastAPI:
    app = FastAPI(title="LocalSTT", version="0.1.0")

    @app.get("/health")
    def health() -> dict:
        return service.health()

    @app.get("/metrics")
    def metrics() -> dict:
        return service.metrics_snapshot()

    @app.post("/v1/audio/transcriptions")
    async def transcriptions(
        file: Annotated[UploadFile, File()],
        model: Annotated[str | None, Form()] = None,
        language: Annotated[str | None, Form()] = None,
        response_format: Annotated[str, Form()] = "json",
    ):
        suffix = Path(file.filename or "audio.wav").suffix or ".wav"
        with tempfile.NamedTemporaryFile(delete=False, suffix=suffix) as tmp:
            tmp.write(await file.read())
            tmp_path = Path(tmp.name)
        try:
            if model and model != service.config.model:
                service.logger.warning("API requested model %s but loaded model is %s", model, service.config.model)
            result = service.transcribe(tmp_path, language=language, response_format=response_format)
        finally:
            try:
                tmp_path.unlink(missing_ok=True)
            except OSError:
                pass

        if response_format == "text":
            return PlainTextResponse(result.text)
        if response_format != "json":
            return JSONResponse({"error": f"unsupported response_format: {response_format}"}, status_code=400)
        return result_to_json(result)

    return app
