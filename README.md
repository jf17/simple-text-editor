# Simple Text Editor
GTK3 and Glade

## CI/CD

На push/PR в `main` GitHub Actions собирает `.deb` и сохраняет его как artifact.
При теге `v*` пакет также прикрепляется к GitHub Release:

```bash
git tag v1.0.0
git push origin v1.0.0
```

## Dependencies

```bash
sudo apt install clang libgtk-3-dev pkg-config make dpkg-dev fakeroot -y
```

Проверка:

```bash
make deps
```

## Help

```bash
make
```

## Build

```bash
make all
```

## Run

```bash
make run
```

или

```bash
./simple-text-editor
```

## Deb package

```bash
make deb
```

Установка:

```bash
sudo apt install ./simple-text-editor_1.0.0_amd64.deb
```

## Clean

```bash
make clean
```
