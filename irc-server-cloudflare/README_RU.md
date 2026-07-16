# Бесплатный NL IRC через Cloudflare Workers + D1

GitHub используется только для публичного файла `irc/irc-config.json`. Сам чат нельзя надёжно хранить в GitHub: для отправки сообщений потребовался бы секретный GitHub-токен внутри JAR, а это небезопасно. Сообщения и online-сессии обслуживает Cloudflare Worker; бесплатного тарифа достаточно для небольшого клиентского чата.

## 1. Подготовка

1. Создай бесплатный аккаунт на https://dash.cloudflare.com/.
2. Установи Node.js 20 или новее.
3. Открой PowerShell в папке `irc-server-cloudflare`.

```powershell
npm install
npx wrangler login
```

## 2. Создание бесплатной базы D1

```powershell
npx wrangler d1 create neverlose-irc
```

Команда напечатает `database_id`. Вставь его в `wrangler.toml` вместо:

```text
PASTE_D1_DATABASE_ID_HERE
```

Создай таблицы:

```powershell
npx wrangler d1 execute neverlose-irc --remote --file=./schema.sql
```

## 3. Секрет для защиты IP-хэшей

```powershell
npx wrangler secret put IP_SALT
```

Вставь длинную случайную строку, например результат этой PowerShell-команды:

```powershell
-join ((1..64) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) })
```

Секрет нельзя добавлять в GitHub.

## 4. Публикация сервера

```powershell
npx wrangler deploy
```

Wrangler покажет адрес наподобие:

```text
https://neverlose-irc.ТВОЙ-SUBDOMAIN.workers.dev
```

Проверь:

```text
https://neverlose-irc.ТВОЙ-SUBDOMAIN.workers.dev/v1/health
```

Должен открыться JSON с `"ok": true`.

## 5. Подключение клиента через GitHub

В файле `irc/irc-config.json` замени `apiUrl`:

```json
{
  "enabled": true,
  "apiUrl": "https://neverlose-irc.ТВОЙ-SUBDOMAIN.workers.dev/v1",
  "message": "IRC is temporarily unavailable"
}
```

Добавь папки в репозиторий `stepanpeep/neverlose-loader`:

```powershell
git add irc irc-server-cloudflare
git commit -m "Add NL IRC server"
git pull --rebase origin main
git push
```

Клиент читает конфигурацию по адресу:

```text
https://raw.githubusercontent.com/stepanpeep/neverlose-loader/main/irc/irc-config.json
```

Адрес Worker можно менять без пересборки JAR. Для временного отключения IRC поставь `"enabled": false` и измени `message`.

## 6. Использование в Minecraft

После сборки нового клиентского JAR:

```text
.irc toggle
.irc chat Привет!
.irc list
```

## Что уже защищено

- только HTTPS endpoint;
- сервер сам берёт автора из session token, а не из отправленного сообщения;
- токен хранится только в памяти клиента и истекает через 24 часа;
- не более 240 символов;
- задержка минимум 2 секунды между сообщениями;
- не более 8 активных сессий с одного IP-хэша;
- сообщения хранятся 7 дней;
- сетевые операции выполняются в daemon-thread;
- вывод в Minecraft выполняется через главный client thread;
- таймауты запросов и автоматическое переподключение.

## Ограничение идентификации

Ник Minecraft в этой бесплатной версии не подтверждается Microsoft OAuth, поэтому абсолютной защиты от подмены ника нет. Для подтверждённых аккаунтов нужен отдельный OAuth/login backend. Не помещай Cloudflare API tokens, GitHub tokens или другие секреты внутрь JAR.
