import logging
import uvicorn
from .config import load_settings
from .web import create_app

def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    settings = load_settings()
    uvicorn.run(create_app(settings), host=settings.app_host, port=settings.app_port, access_log=False)

if __name__ == "__main__": main()
