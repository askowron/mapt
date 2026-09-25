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
git tag -s v0.1.0 -m 'mapt 0.1.0'
git push origin v0.1.0
```

Można też uruchomić workflow ręcznie z zakładki **Actions**.

Dla repozytorium `askowron/mapt` domyślny adres będzie:

```text
https://askowron.github.io/mapt/
```

Adres finalny można odczytać z kroku deploymentu GitHub Actions.

## Dodanie repozytorium na komputerze

```sh
sudo install -d /etc/apt/keyrings

curl -fsSL https://askowron.github.io/mapt/mapt-archive-keyring.gpg \
  | sudo tee /etc/apt/keyrings/mapt-archive-keyring.gpg >/dev/null

echo 'deb [signed-by=/etc/apt/keyrings/mapt-archive-keyring.gpg] https://askowron.github.io/mapt stable main' \
  | sudo tee /etc/apt/sources.list.d/mapt.list

sudo apt update
sudo apt install mapt
```

Prywatny klucz GPG powinien pozostać poza repozytorium i być przechowywany
w bezpiecznym miejscu. Każda kolejna wersja wymaga nowego tagu i podpisanego
`Release`.
