"""The shipped model configuration, read from shipped-model.json (see scripts/evaluate_variants.py)."""
import json
from bootstrap import ROOT
CONFIG = json.loads((ROOT / 'shipped-model.json').read_text('utf-8'))
MODEL_DIR = ROOT / CONFIG['model_dir']
REFERENCE_DIR = ROOT / CONFIG['reference_dir']
VARIANT = CONFIG['variant']
def remote(key):
    return json.loads((ROOT / 'remote-assets.lock.json').read_text('utf-8'))[CONFIG[key]]
