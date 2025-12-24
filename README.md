# ArcXSFV

**ArcXSFV** est un outil industriel de vérification d’intégrité de fichiers.  
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
- Débit mesuré : **~40.58 GB/s** (ReleaseFast)

### Interface – C++ / Win32

- Drag & Drop fichiers / dossiers
- Indication d’état :
  - Vert : valide
  - Rouge : corrompu
  - Bleu : en cours




