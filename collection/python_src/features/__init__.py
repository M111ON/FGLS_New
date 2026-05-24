from __future__ import annotations
import sys, importlib, pkgutil, inspect, logging
from pathlib import Path
from typing import List
from .base import EngineFeature

logger = logging.getLogger("engine.features")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
if str(_COLLECTION) not in sys.path:
    sys.path.insert(0, str(_COLLECTION))
if str(_HERE.parent) not in sys.path:
    sys.path.insert(0, str(_HERE.parent))


def discover_features() -> List[EngineFeature]:
    features: List[EngineFeature] = []
    for importer, mod_name, is_pkg in pkgutil.iter_modules([str(_HERE)]):
        if mod_name == "base" or is_pkg:
            continue
        try:
            mod = importlib.import_module(f"features.{mod_name}")
            for name in dir(mod):
                obj = getattr(mod, name)
                if (inspect.isclass(obj) and issubclass(obj, EngineFeature)
                        and obj is not EngineFeature):
                    instance = obj()
                    features.append(instance)
                    logger.info(f"  Feature loaded: {instance.name}")
        except Exception as e:
            logger.warning(f"  Feature {mod_name} failed: {e}")
    return features
