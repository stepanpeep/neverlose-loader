# GitHub: публикация и рабочая сборка Neverlose Loader

## 0. Что уже настроено

Проект собирается нативно под **Windows x64** через CMake и Visual Studio. Workflow `.github/workflows/build-windows.yml` выполняет чистую Release-сборку с `/W4 /WX`, создаёт ZIP и при теге `v*` публикует GitHub Release.

Лаунчер получает Minecraft 1.21 из Mojang, Fabric Loader из Fabric Meta, моды из Modrinth и проверяет SHA-хэши. Для online-mode серверов потребуется отдельный Microsoft OAuth; встроенный профиль сейчас локальный/offline.

## 1. Установка на ПК разработчика

В Visual Studio Installer включи workload **Desktop development with C++** и компоненты:

- MSVC v143 x64/x86;
- Windows 10/11 SDK;
- CMake tools for Windows;
- Git for Windows.

Также установи **Java 21 x64** и задай `JAVA_HOME`, либо положи Java в `<папка клиента>/runtime/bin/javaw.exe`.

Чистая локальная сборка:

```powershell
Remove-Item build -Recurse -Force -ErrorAction SilentlyContinue
cmake -S . -B build -A x64 -DNL_WARNINGS_AS_ERRORS=ON
cmake --build build --config Release --clean-first
```

Готовый файл: `build\Release\NeverloseLoader.exe`. Рядом CMake автоматически создаст `manifest\manifest.example.json`.

## 2. Создание репозитория

1. Открой `https://github.com/new`.
2. Название: `neverlose-loader`.
3. Выбери Public или Private.
4. Не создавай README/.gitignore на сайте — они уже есть в архиве.

В PowerShell из корня проекта:

```powershell
git init
git branch -M main
git add .
git commit -m "Initial Neverlose Loader"
git remote add origin https://github.com/USERNAME/neverlose-loader.git
git push -u origin main
```

Замени `USERNAME`. Если GitHub запрашивает пароль, используй вход через браузер/Git Credential Manager или Personal Access Token, а не пароль аккаунта.

## 3. Проверка GitHub Actions

Открой **Actions → Build Neverlose Loader**. Зелёными должны быть Configure, Build, Package и Upload artifact. ZIP появится в блоке Artifacts.

Если Actions отключены: **Settings → Actions → General → Allow all actions**. Для автоматического Release workflow уже содержит `contents: write`.

## 4. Удалённый manifest

После push manifest доступен по адресу:

```text
https://raw.githubusercontent.com/USERNAME/neverlose-loader/main/manifest/manifest.example.json
```

В лаунчере: **Settings → Manifest URL → Refresh**. URL сохраняется в `%LOCALAPPDATA%\NeverloseLoader\launcher.ini`.

Чтобы URL был стандартным для всех новых установок, замени его одновременно в:

- `src/Models.h` (`manifestUrl`);
- `src/SettingsService.cpp` (оба fallback-значения).

После изменения пересобери EXE.

## 5. Моды Modrinth

Пример модуля:

```json
{
  "id": "fabric-api",
  "name": "Fabric API",
  "description": "Required Fabric runtime API",
  "modrinthProject": "fabric-api",
  "artifacts": []
}
```

`modrinthProject` — slug из `modrinth.com/mod/SLUG`. Лаунчер фильтрует версии по `fabric` и `1.21`, берёт primary-файл и проверяет SHA-512. Добавь `id` мода в нужные `presets`.

Файлы, скачанные самим лаунчером, учитываются в `mods/.neverlose-managed.txt`: старые версии управляемых модов удаляются, а пользовательские JAR не затрагиваются.

## 6. Собственный клиентский JAR

Крупный JAR публикуй в GitHub Release, не через Raw. Посчитай SHA-256:

```powershell
Get-FileHash .\NeverloseClient.jar -Algorithm SHA256
```

Добавь в `versions[0].artifacts`:

```json
{
  "path": "mods/NeverloseClient.jar",
  "url": "https://github.com/USERNAME/neverlose-loader/releases/download/client-1.0.0/NeverloseClient.jar",
  "sha256": "64_HEX_SYMBOLS"
}
```

Путь обязан быть относительным и не содержать `..`. При несовпадении SHA-256 запуск блокируется.

## 7. Выпуск версии

```powershell
git add .
git commit -m "Release 1.0.0"
git push
git tag v1.0.0
git push origin v1.0.0
```

Workflow соберёт `NeverloseLoader-windows-x64.zip` и приложит его к Release.

## 8. Maintenance mode

```json
"maintenance": {
  "enabled": true,
  "message": "Обновление клиента. Попробуйте позже."
}
```

После commit/push пользователи увидят сообщение при Refresh, а запуск будет заблокирован.

## 9. Диагностика

- **Старые ошибки `offlineUuid/gameInstaller_`**: удали `build` полностью и пересобери; в текущем `LauncherCore.h` объявления уже исправлены.
- **Java не найдена**: проверь `java -version` (нужна 21), `JAVA_HOME`, либо `runtime/bin/javaw.exe`.
- **HTTP/SHA error**: проверь URL релиза и хэш; не используй HTML-страницу вместо прямой ссылки на файл.
- **Modrinth no compatible version**: проект не опубликовал Fabric-сборку именно для 1.21 — выбери другой slug/артефакт.
- **GitHub Actions красный**: открой первую строку ошибки Build; предупреждения намеренно считаются ошибками, чтобы релиз не собирался с потенциальными дефектами.
