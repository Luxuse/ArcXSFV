# ArcaXSFV

**ArcaXSFV** est un outil industriel de vérification d’intégrité de fichiers.  
Il associe un moteur de hachage haute performance (**ArcaHash**, Zig) à une interface multithreadée native (**C++ Win32**).

## Caractéristiques

- **I/O haute performance** : lecture par *memory mapping* (`MapViewOfFile`)
- **Architecture hybride** :
  - Zig : moteur de hachage (sécurité, performances, contrôle mémoire)
  - C++ Win32 : interface native, faible overhead
- **Multithreading natif** : parallélisation complète sur tous les cœurs CPU
- **Format `.arca`** : format binaire compact pour signatures de fichiers et répertoires

## Spécifications techniques

### Moteur – Zig (ArcaHash)

- Traitement par blocs de **32 octets**
- Accumulateurs doubles
- Multiplications **128 bits**
- Chemin critique optimisé (*branchless*)
- Débit mesuré ram:  
Benchmark Suite
Testing 128.0 MB
 Hash:       0x3d59d742ce85b3fb
 Iterations: 10

 Avg Time:   2.844 ms
 Min Time:   2.542 ms
 Max Time:   3.239 ms

 Avg Speed:  43.96 GB/s
Peak Speed: 49.17 GB/s
B/cycle:    10.49 @ 4.5GHz


Testing 512.0 MB
 Hash:       0xa76812a1f0b5604c
 Iterations: 10

 Avg Time:   11.449 ms
 Min Time:   10.721 ms
 Max Time:   12.228 ms

 Avg Speed:  43.67 GB/s
 Peak Speed: 46.64 GB/s
 B/cycle:    10.42 @ 4.5GHz

- Débit mesuré : **~2.6 GB/s** (In gen 3 nvme)

### Interface – C++ / Win32

- Drag & Drop fichiers / dossiers
- Suivi temps réel (auto-scroll)
- Indication d’état :
  - Vert : valide
  - Rouge : corrompu
  - Bleu : en cours


