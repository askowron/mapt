# Publikacja repozytorium APT przez GitHub Pages

Workflow `.github/workflows/publish-apt.yml` buduje pakiet `mapt`, tworzy
indeks `Packages`, generuje `Release`, podpisuje repozytorium kluczem GPG
i publikuje pliki na GitHub Pages.

## Konfiguracja GitHuba

W repozytorium ustaw **Settings → Pages → Source → GitHub Actions**.

Dodaj następujące sekrety:

- `GPG_PRIVATE_KEY` — armored prywatny klucz GPG;
- `GPG_PASSPHRASE` — hasło klucza, jeśli jest chroniony;
- `GPG_KEY_ID` — odcisk palca klucza; opcjonalne, jeśli klucz jest jeden.

Prywatnego klucza nie umieszczaj w repozytorium. Jego publiczny eksport
pojawi się automatycznie w `mapt-archive-keyring.gpg` po publikacji.

Przykładowe przygotowanie klucza:

```sh
gpg --full-generate-key
gpg --armor --export-secret-keys --pinentry-mode loopback \
  --passphrase 'HASLO' > mapt-private.asc
```

## Publikacja wersji

Workflow uruchamia się po utworzeniu tagu `v*`:

```sh
git tag -s v0.1.2 -m 'mapt 0.1.2'
git push origin v0.1.2
```

Można też uruchomić workflow ręcznie z zakładki **Actions**.

Dla repozytorium `askowron/mapt` domyślny adres będzie:

```text
https://askowron.github.io/mapt/
```

Adres finalny można odczytać z kroku deploymentu GitHub Actions.

## Znane ograniczenie: tylko jedna wersja

Każdy przebieg buduje `pool/` od zera, więc repozytorium zawiera wyłącznie
najnowszą wersję. Poprzednie pliki `.deb` nie są archiwizowane.

Konsekwencja: APT musi pobrać zainstalowaną wersję, żeby ją zastąpić. Maszyna,
na której jest starsza wersja, nie zaktualizuje się przez `apt upgrade` — pliku
nie ma w `pool/`. Upgrade wymaga usunięcia pakietu i ponownej instalacji:

```sh
sudo apt remove mapt
sudo apt install mapt
```

Świeże instalacje i reinstalacje działają normalnie.

## Dodanie repozytorium na komputerze

Klucz publiczny jest publikowany w formacie ASCII-armored. APT oczekuje
binarnego keyringu, więc plik trzeba przed użyciem zdearmorować przez
`gpg --dearmor`. Bez tego kroku `apt update` kończy się błędem
`NO_PUBKEY E1C3C1739078B38F`.

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

Prywatny klucz GPG powinien pozostać poza repozytorium i być przechowywany
w bezpiecznym miejscu. Każda kolejna wersja wymaga nowego tagu i podpisanego
`Release`.
