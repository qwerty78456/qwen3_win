"""The shipped model configuration, read from shipped-model.json (see scripts/evaluate_variants.py).

`MODELS['default']` is the configuration that loads without flags; `MODELS['large']` is the optional
GPU-only model (`AsrWin.exe --large-model`) described under `directml.large_model`.
"""
import json
from bootstrap import ROOT
CONFIG = json.loads((ROOT / 'shipped-model.json').read_text('utf-8'))
MODEL_DIR = ROOT / CONFIG['model_dir']
REFERENCE_DIR = ROOT / CONFIG['reference_dir']
VARIANT = CONFIG['variant']
DIRECTML = CONFIG.get('directml', {})
DIRECTML_VALIDATED = bool(DIRECTML.get('validated'))
MODELS = {'default': {k: CONFIG[k] for k in ('name', 'configuration', 'model_dir', 'variant', 'threads', 'onnx_lock_key', 'reference_lock_key', 'reference_dir')}}
MODELS['default'].update(regression_manifest='regression/manifest.json', evaluation_reference_dir='evaluation/reference', suffix='')
if DIRECTML.get('large_model'):
    MODELS['large'] = dict(DIRECTML['large_model'])
    MODELS['large'].setdefault('suffix', '-' + MODELS['large']['name'])
def model_config(key='default'):
    """Configuration dictionary for `default` or `large` (raises when the large model is not configured)."""
    if key not in MODELS: raise KeyError(f'Model configuration {key!r} is not present in shipped-model.json')
    return MODELS[key]
def remote(key, model='default'):
    """Publisher lock entry named by the configuration's `key` (onnx_lock_key / reference_lock_key)."""
    return json.loads((ROOT / 'remote-assets.lock.json').read_text('utf-8'))[model_config(model)[key]]
