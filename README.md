# mapt

**mapt** — menadżer pakietów oparty o `apt`: dwa panele, pasek menu,
klawisze F1–F10 i kolorowy, pełnoekranowy interfejs terminalowy.
Napisany w C11 z użyciem **ncurses**.

```
 mapt 0.1.1                      14:32
 File  Mark  Command  Options  Help
┌ Installed ──────────────(1885)┐┌ Available ────────────(84845)┐
│ Package     Version          S││ Package     Version          O │
│ [+] 7zip    23.01+dfsg-11    6M││ 0ad         0.0.26-6ubuntu  u │
│ acpid       2.5.6-1ubuntu7   4││ 0ad-data    0.0.26-6ubuntu  u │
│ adduser     3.137ubuntu1     8││ 0ad-data-c… 0.0.26-6ubuntu  u │
│ ...                             ││ ...                              │
└ srt:name/asc     marks:0 ──────┘└ srt:name/asc     marks:0 ───────┘
 7zip  23.01+dfsg-11  amd64  6,0 M                        1/1885
1Help 2Menu 3Info 4List 5Inst 6Upgr 7Find 8Del 9Menu 10Quit
```

## Dwa panele

| Panel | Zawartość | Kolumny |
|-------|-----------|---------|
| lewy  | pakiety **zainstalowane** (status `ii` z `dpkg-query`); opcja w menu **Options** zawęża go do pakietów z aktualizacją | `Package`, `Version`, `Size` |
| prawy | pakiety **niezainstalowane** — reszta cache'u APT | `Package`, `Version`, `Origin` |

Panele są ścisłą partycją bazy: pakiet zainstalowany nigdy nie
pojawia się po prawej stronie i odwrotnie.

Kolory: biało-na-niebieskim zwykły pakiet, czarno-na-cyanie kursor,
żółty oznaczenie zaznaczenia, zielony — dostępna aktualizacja
(w lewym panelu), cyjan — zainstalowane zależności w drzewie.

## Drzewo zależności

Pakiet mający `Depends`/`PreDepends` pokazuje przed nazwą znacznik
`[+]` (zwinięte) lub `[-]` (rozwinięte):

```
│ [+] 7zip     23.01+dfsg-11     6 M│
│     [-] dbus    1.14-10ubuntu...│   znacznik i nazwa przesunięte
│         libc6   2.39-0ubuntu...│   w prawo o jeden tab (4 kolumny)
│                                  │   na poziom; zainstalowane
│                                  │   świecą na cyjan
```

- `Enter` rozwija lub zwija, `→` rozwija, `←` zwija,
- **`Tab` lub klik panelu przełącza aktywny panel** — strzałki tego
  już nie robią,
- zależności widocznych wierszy są pobierane zawczasu
  (`apt-cache depends --no-recommends ...`), a rozwinięte węzły
  pamiętają stan także po przeładowaniu bazy,
- obejmuje tylko `Depends` i `PreDepends`; pakiety wirtualne
  pokazywane są przez swoich providerów.

## Budowanie

Wymagania: `gcc` (lub inny C11), `make`, nagłówki `libncurses-dev`.

```sh
sudo apt-get install -y build-essential libncurses-dev
make            # buduje ./mapt
make test       # testy jednostkowe (porównywanie wersji wg dpkg)
make install    # instaluje do /usr/local/bin + stronę man
./mapt
```

`mapt` czyta dane tylko przez podprocesy (`dpkg-query -W`, `apt list`,
`apt-cache policy`) — niczego nie modyfikuje bez pytania, a wszystkie
operacje zapisujące przechodzą przez `sudo` z potwierdzeniem.

## Publikacja APT

Repozytorium APT jest publikowane przez GitHub Pages po utworzeniu tagu
`v*`. Szczegóły konfiguracji klucza GPG, sekretów GitHub i adresu klienta
znajdują się w [`doc/APT-REPOSITORY.md`](doc/APT-REPOSITORY.md).

Instalacja z repozytorium:

```sh
sudo install -d /etc/apt/keyrings
curl -fsSL https://askowron.github.io/mapt/mapt-archive-keyring.gpg \
  | sudo gpg --dearmor --yes \
      --output /etc/apt/keyrings/mapt-archive-keyring.gpg
echo 'deb [signed-by=/etc/apt/keyrings/mapt-archive-keyring.gpg] https://askowron.github.io/mapt stable main' \
  | sudo tee /etc/apt/sources.list.d/mapt.list
sudo apt update
sudo apt install mapt
```

Repozytorium przechowuje tylko najnowszą wersję, więc `apt upgrade` zadziała
tylko dla maszyn bez zainstalowanego `mapt`. Przy zmianie wersji trzeba pakiet
usunąć i zainstalować ponownie — szczegóły w
[`doc/APT-REPOSITORY.md`](doc/APT-REPOSITORY.md).

## Skróty klawiszowe

### Nawigacja

| Klawisz | Akcja |
|---------|-------|
| `↑` `↓` `PgUp` `PgDn` `Home` `End` | ruch kursora |
| `Tab` lub klik panelu | przełączenie aktywnego panelu |
| `Enter`, `→`, `←` | rozwijanie/zwijanie drzewa zależności |
| dowolny znak, `Backspace`, `Esc` | wyszukiwanie skokowe w panelu |
| `F3` | informacje o pakiecie |
| `F4` | lista plików (`dpkg -L`) lub plik archiwum |
| `F7` | szukanie po fragmencie nazwy |

### Mysz

| Obsługa | Działanie |
|---------|-----------|
| lewy klik w panelu | aktywuje panel i wybiera wskazany pakiet |
| podwójny klik | rozwija lub zwija drzewo zależności |
| kółko myszy | przewija panele, podglądy tekstu i okno poleceń |
| klik paska menu, klawiszy funkcyjnych i przycisków | wywołuje odpowiednią akcję |
| klik obszaru paska przewijania | przeskakuje do wybranego miejsca listy |

### Zaznaczanie i akcje

| Klawisz | Akcja |
|---------|-------|
| `Insert` | przełącz zaznaczenie (i idź w dół) |
| `+` | zaznacz wg wzorca (glob, np. `lib*`) |
| `-` | skasuj zaznaczenia w panelu |
| `F5` | instalacja / reinstalacja zaznaczonych |
| `F6` | aktualizacja zaznaczonych |
| `F8`, `Delete` | usunięcie zaznaczonych |
| `Ctrl+R` | przeładowanie bazy pakietów |

### Globalne

| Klawisz | Akcja |
|---------|-------|
| `F1` | pomoc |
| `F2`, `F9` | pasek menu (działa z kursorami `←`/`→`) |
| `F10` | zakończenie |

Menu **Command** zawiera dodatkowo: aktualizację list pakietów
(`apt-get update`), aktualizację wszystkich pakietów (`apt-get upgrade`)
i czyszczenie zależności. Menu **Options** przełącza sortowanie
(nazwa/wersja/rozmiar, rosnąco/malejąco) oraz zawartość lewego panelu
(wszystkie zainstalowane / tylko z aktualizacją).

## Jak to działa

1. **`dpkg-query -W -f=...`** daje prawdziwy status (tylko wpisy `ii`
   trafiają do lewego panelu), wersję zainstalowaną, architekturę
   i `Installed-Size` — z tego żyje kolumna `Size`.
2. **`apt list`** (~92 tys. linii, ~2 s) — kolumna wersji to kandydat
   (tak drukuje ją apt w `ListSingleVersion()`), a wiersz zawiera też
   przybliżone repozytorium (lista kieszeni), architekturę oraz flagę
   `[upgradable from: ...]`. To baza obu paneli. Całej bazy nie da się
   zapytać przez `apt-cache policy`: potrzeba ~87 tys. argumentów
   i ~25 sekund.
3. Widoczne wiersze (plus 100 wierszy wyprzedzenia) doprecyzowuje
   `apt-cache policy` — dokładny origin pojawia się w kolumnie `Origin`
   i na pasku stanu, a odpowiedzi są pamiętane. Zapytania (`policy`
   i `depends`) ruszają dopiero po zatrzymaniu klawiatury: dopóki
   naciskasz klawisze, nic nie czeka na `apt-cache`, więc przewijanie
   reaguje natychmiast, a pierwszy widok pobierany jest zaraz po
   pierwszym malowaniu. Start nie czeka na `policy`: baza jest gotowa
   w ~2–3 s. Z zależności widocznych wierszy (`apt-cache depends`)
   żyje drzewo pakietów.
4. Flaga aktualizacji: `dpkg_vercmp(kandydat, zainstalowana) > 0`
   (`src/vercmp.c`, ten sam algorytm co w `dpkg --compare-versions`)
   plus sygnał `[upgradable from: ...]` z `apt list`.
5. Szczegóły pakietu (`apt-cache show`, `apt-cache policy`,
   `apt-cache depends`, `dpkg -L`) są pobierane dopiero przy otwarciu
   okna — interfejs startuje od razu.
6. Operacje na pakietach: `sudo -v` pyta o hasło raz (pole bez podglądu),
   potem `sudo -n apt-get ...`, a wyjście leci do przewijanego okna
   z paskiem przewijania, obsługą `Esc` (przerwanie, `SIGTERM`)
   i statusem zakończenia. Gdy dpkg lub apt podają odsetki
   (`Progress:`, `Reading database ... NN%`), w oknie rysowany jest
   pasek postępu `[████░░░░] NN%`, a takie linie nie trafiają do
   logu — idą wyłącznie na pasek. Bez odsetków (`apt-get update`)
   wyświetla się animowany spinner `⠄ working...`. Okno startowego
   ładowania bazy również pokazuje spinner przy aktualnej
   wiadomości kroku.

## Struktura projektu

```
mapt/
├── Makefile
├── README.md
├── doc/mapt.1            strona man
├── src/
│   ├── main.c            inicjalizacja, pętla zdarzeń, komendy
│   ├── ui.[ch]           kolory, ramki, prostokąty
│   ├── panel.[ch]        rysowanie paneli, sortowanie, szukanie, znaczniki
│   ├── dialog.[ch]       modalne okna (alert, potwierdzenie, hasło,
│   │                     przewijany tekst, menu, okno poleceń)
│   ├── menu.[ch]         pasek menu + rozwijane pozycje
│   ├── apt.[ch]          sudo, wywołania apt-get, szczegóły pakietu
│   ├── pkg.[ch]          baza pakietów + parsery policy/dpkg-query/apt list
│   ├── proc.[ch]         fork/exec z dwoma rurami, linia po linii
│   ├── vercmp.[ch]       porównywanie wersji wg dpkg
│   ├── util.[ch]          bufory, argv, formatowanie rozmiarów i czasu
│   ├── cmd.h             katalog komend
│   └──version.h
├── tests/
│   ├── test_vercmp.c          testy jednostkowe
│   └── test_vercmp_dpkg.c     test różnicowy vs dpkg --compare-versions
└── ...
```

## Autor

APPIT Adam Skowroński <info@appit.pl>

## Licencja

MIT — pełny tekst w [`LICENSE`](LICENSE). Pliki źródłowe oznaczają licencję
nagłówkiem `SPDX-License-Identifier: MIT`.
