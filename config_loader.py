#!/usr/bin/env python3
"""
Configuration loader for HFT Infrastructure Lab.
Moduł ładowania konfiguracji dla laboratorium infrastruktury HFT.

Loads settings from config.yaml with environment variable overrides.
Ładuje ustawienia z config.yaml z możliwością nadpisania zmiennymi środowiskowymi.
"""
import os
import logging
from typing import Any, Dict, Optional

# Try to import the yaml library (for reading YAML config files)
# If it fails (library not installed), set yaml to None as a fallback
# This is like 'command || echo failed' in bash - try something, handle the error gracefully
try:
    import yaml
except ImportError:
    # ImportError means the yaml module is not installed/available
    # Instead of crashing, we set it to None and handle it later
    yaml = None  # type: ignore


_DEFAULT_CONFIG: Dict[str, Any] = {
    'risk': {
        'max_position_per_symbol': 5000,
        'max_portfolio_exposure': 50000,
        'max_daily_loss': 100000.0,
        'max_orders_per_second': 1000,
        'max_order_value': 500000.0,
        'max_drawdown_pct': 5.0,
    },
    'oms': {
        'max_position': 10000,
        'max_order_value': 1000000.0,
    },
    'strategy': {
        'window': 20,
        'threshold_pct': 0.1,
        'order_size': 100,
    },
    'router': {
        'default_strategy': 'BEST_PRICE',
        'split_threshold': 500,
        'venues': [
            {'name': 'NYSE', 'latency_ns': 500, 'fee_per_share': 0.003},
            {'name': 'NASDAQ', 'latency_ns': 200, 'fee_per_share': -0.002},
            {'name': 'BATS', 'latency_ns': 150, 'fee_per_share': -0.001},
        ],
    },
    'simulator': {
        'num_messages': 10000,
        'seed': 42,
    },
    'logging': {
        'level': 'INFO',
        'format': '%(asctime)s [%(levelname)s] %(name)s: %(message)s',
        'date_format': '%H:%M:%S',
    },
}

_config: Optional[Dict[str, Any]] = None


def _find_config_file() -> Optional[str]:
    """Search for config.yaml in project root.
    Szuka config.yaml w katalogu głównym projektu.
    """
    # Check env var first / Najpierw sprawdź zmienną środowiskową
    env_path = os.environ.get('HFT_CONFIG')
    if env_path and os.path.isfile(env_path):
        return env_path

    # Walk up from this file to find config.yaml
    # Szukaj config.yaml idąc w górę od tego pliku
    current = os.path.dirname(os.path.abspath(__file__))
    for _ in range(5):
        candidate = os.path.join(current, 'config.yaml')
        if os.path.isfile(candidate):
            return candidate
        current = os.path.dirname(current)
    return None


def load_config(path: Optional[str] = None) -> Dict[str, Any]:
    """Load configuration from YAML file with fallback to defaults.
    Ładuje konfigurację z pliku YAML z domyślnymi wartościami.
    """
    # 'global' keyword declares that we're modifying the _config variable defined outside this function
    # Without 'global', assigning to _config would create a new local variable instead
    # Like declaring 'export' in bash to make a variable available to child shells/functions
    global _config
    if _config is not None and path is None:
        return _config

    config = dict(_DEFAULT_CONFIG)

    config_path = path or _find_config_file()
    if config_path and yaml is not None:
        try:
            with open(config_path, 'r') as f:
                # yaml.safe_load() reads the YAML text file and converts it into a Python dictionary
                # YAML is a human-readable config format; safe_load parses it safely
                # Similar to 'cat config.yaml | parse' - we read text and convert it to structured data
                file_config = yaml.safe_load(f) or {}
            # Deep merge file config into defaults
            # Głębokie scalanie konfiguracji z pliku z domyślnymi
            for section, values in file_config.items():
                if section in config and isinstance(values, dict):
                    config[section] = {**config[section], **values}
                else:
                    config[section] = values
        except Exception as e:
            logging.getLogger('config').warning(f"Failed to load {config_path}: {e}, using defaults")

    # Apply environment variable overrides (HFT_RISK_MAX_DAILY_LOSS=50000)
    # Zastosuj nadpisania ze zmiennych środowiskowych
    for key, value in os.environ.items():
        if key.startswith('HFT_'):
            parts = key[4:].lower().split('_', 1)
            if len(parts) == 2 and parts[0] in config:
                section, param = parts
                if isinstance(config[section], dict) and param in config[section]:
                    original = config[section][param]
                    try:
                        # isinstance() checks the type of a value (like 'test -f' checks if something is a file in bash)
                        # This checks if the original config value is a float type before converting
                        if isinstance(original, float):
                            config[section][param] = float(value)
                        elif isinstance(original, int):
                            config[section][param] = int(value)
                        else:
                            config[section][param] = value
                    except ValueError:
                        pass

    _config = config
    return config


def get_section(section: str) -> Dict[str, Any]:
    """Get a specific config section.
    Pobierz konkretną sekcję konfiguracji.
    """
    config = load_config()
    return dict(config.get(section, {}))


def setup_logging(config: Optional[Dict[str, Any]] = None) -> None:
    """Configure Python logging from config.
    Konfiguruj logowanie Pythona z pliku konfiguracyjnego.
    """
    cfg = config or get_section('logging')
    # getattr(object, 'attribute_name', default) dynamically accesses an attribute by name
    # Like variable variable names in bash: eval "var_$name" - we build the attribute name at runtime
    # Here it looks up the logging level (e.g., 'INFO' → logging.INFO) using the string from config
    level = getattr(logging, cfg.get('level', 'INFO').upper(), logging.INFO)
    fmt = cfg.get('format', '%(asctime)s [%(levelname)s] %(name)s: %(message)s')
    datefmt = cfg.get('date_format', '%H:%M:%S')

    logging.basicConfig(level=level, format=fmt, datefmt=datefmt)
