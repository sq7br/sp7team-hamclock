Import("env")
import os
import csv

# Automatyczna instalacja brakującej biblioteki intelhex, wymaganej przez nowsze wersje esptool
try:
    import intelhex
except ImportError:
    print("--- Instalowanie brakującej biblioteki 'intelhex' ---")
    env.Execute("$PYTHONEXE -m pip install intelhex")

def get_spiffs_offset(csv_path):
    """Funkcja wyciągająca adres SPIFFS z pliku CSV."""
    try:
        with open(csv_path, mode='r') as f:
            reader = csv.reader(f, skipinitialspace=True)
            for row in reader:
                if not row or row[0].startswith('#'): continue
                if 'spiffs' in row[0].lower() or 'storage' in row[0].lower():
                    return row[3].strip() # Zwraca wartość z kolumny Offset
    except FileNotFoundError:
        print(f"OSTRZEŻENIE: Plik partycji nie został znaleziony w {csv_path}")
        return None
    return None # Zwróć None, jeśli nie znaleziono

def merge_action(source, target, env):
    """Akcja, która wykonuje scalanie plików binarnych."""
    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions_bin = os.path.join(build_dir, "partitions.bin")
    firmware = source[0].get_path() # Pobieramy ścieżkę do firmware.bin z zależności
    spiffs = os.path.join(build_dir, "spiffs.bin")
    merged_output = target[0].get_path()

    csv_path = os.path.join(project_dir, "partitions.csv")
    spiffs_offset = get_spiffs_offset(csv_path)

    if not spiffs_offset:
        print("BŁĄD: Nie można określić offsetu SPIFFS. Przerywam scalanie.")
        return 1 # Zwróć błąd

    print(f"--- Automatyczne scalanie: Wykryto offset SPIFFS: {spiffs_offset} ---")

    # Sprawdź, czy wszystkie wymagane pliki istnieją
    for f in [bootloader, partitions_bin, firmware, spiffs]:
        if not os.path.exists(f):
            print(f"BŁĄD: Wymagany plik do scalenia nie istnieje: {f}")
            return 1 # Zwróć błąd

    # Próbujemy użyć narzędzia dostarczonego przez PlatformIO ($UPLOADER)
    esptool_path = env.subst("$UPLOADER")
    
    if esptool_path and os.path.exists(esptool_path):
        esptool_cmd = [esptool_path]
    else:
        # Fallback: jeśli nie znaleziono skryptu, instalujemy moduł i używamy -m
        print("--- OSTRZEŻENIE: Nie znaleziono $UPLOADER, używam modułu python esptool ---")
        env.Execute("$PYTHONEXE -m pip install esptool")
        esptool_cmd = ["-m", "esptool"]

    cmd = [
        env.subst("$PYTHONEXE")] + esptool_cmd + ["--chip", "esp32", "merge_bin",
        "-o", merged_output,
        "--flash_mode", env.subst("$BOARD_FLASH_MODE"),
        "--flash_size", "4MB",
        "0x1000", bootloader,
        "0x8000", partitions_bin,
        "0x10000", firmware,
        spiffs_offset, spiffs
    ]
    
    result = env.Execute(" ".join(cmd))
    if result == 0:
        print(f"--- GOTOWE! Plik do przekazania: {merged_output} ---")
    else:
        print("--- BŁĄD: Scalanie nie powiodło się. ---")
    return result

# Pobieramy nazwy plików z aktualnego środowiska (w fazie POST są one poprawne)
build_dir = env.subst("$BUILD_DIR")
progname = env.subst("$PROGNAME")
firmware_bin = os.path.join(build_dir, f"{progname}.bin")
merged_bin = os.path.join(build_dir, "full_firmware.bin")

# 1. Definiujemy regułę tworzenia full_firmware.bin
#    Jako źródło podajemy firmware.bin, aby SCons wiedział, że musi go najpierw zbudować.
merged_node = env.Command(
    target=merged_bin,
    source=[firmware_bin],
    action=merge_action
)

# 2. Dodajemy zależność od 'buildfs'.
#    To kluczowe: zmusza SCons do uruchomienia celu 'buildfs' (który tworzy spiffs.bin)
#    przed uruchomieniem naszej akcji scalania.
env.Depends(merged_node, "buildfs")

# 3. Ustawiamy nasz scalony plik jako domyślny cel budowania
env.Default(merged_node)