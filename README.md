# Simple Text Editor
GTK3 and Glade

**License:** [Apache License 2.0](LICENSE)

## Возможности

- Новый / Открыть / Сохранить / Сохранить как
- Отмена и повтор
- Вырезать, копировать, вставить, удалить, выделить всё
- Запрос при закрытии несохранённого документа
- Строка состояния (строка и столбец)
- Иконка приложения (`icon.png`)
- Открытие файла из аргумента: `./simple-text-editor файл.txt`
- Drag and drop текстовых файлов в окно редактора

## Горячие клавиши

| Действие | Клавиши |
|---|---|
| Новый | Ctrl+N |
| Открыть | Ctrl+O |
| Сохранить | Ctrl+S |
| Сохранить как | Ctrl+Shift+S |
| Выход | Ctrl+Q |
| Отмена | Ctrl+Z |
| Повтор | Ctrl+Shift+Z / Ctrl+Y |
| Вырезать | Ctrl+X |
| Копировать | Ctrl+C |
| Вставить | Ctrl+V |
| Выделить всё | Ctrl+A |
| Дублировать строку | Ctrl+D |
| Переместить строку | Ctrl+Shift+↑ / Ctrl+Shift+↓ |
| Мультикурсор выше/ниже | Alt+↑ / Alt+↓ |
| Выделение в мультикурсоре | Shift+← / Shift+→ |
| Сбросить мультикурсор | Esc |

## CI/CD

На push/PR в `main` GitHub Actions собирает `.deb` и сохраняет его как artifact.
При теге `v*` пакет также прикрепляется к GitHub Release:

```bash
git tag v1.2.0
git push origin v1.2.0
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
sudo apt install ./simple-text-editor_1.1.0_amd64.deb
```

## Clean

```bash
make clean
```
