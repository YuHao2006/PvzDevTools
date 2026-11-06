import os
from pathlib import Path

def game_root():
    return Path(os.environ.get("PVZ_GAME_ROOT", str(Path(__file__).resolve().parents[4] / "PlantsVsZombiesModern"))).resolve()
