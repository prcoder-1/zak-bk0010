#!/usr/bin/env python3
"""Файлы игры для дискеты: исходные NN.LFL (Zak McKracken Enhanced, DOS) -> правленые копии.

    lfl_patch.py ИСХОДНЫЙ_КАТАЛОГ КАТАЛОГ_КОПИЙ

Исходные файлы не меняются. Правки:

1. Картинка (IM00) каждой комнаты перекодирована тем же RLE v2, но оптимально (разбор
   динамическим программированием): точки те же, байт на 15–25% меньше. Как в
   исходных данных, серия «повтор колонки слева» не начинается в первой колонке полосы
   8 точек (gfx_v2.c: перезапуск декодера с полосы — с пустой dither_table).
2. Блок комнаты должен поместиться в окно страницы СМК (24 Кбайт), файл — в буфер
   загрузчика (tools/zx0lfl.py). Иначе картинка с маской выносятся в свой файл —
   псевдокомнату (59, 60, ...: в индексе Zak таких файлов нет): в заголовке комнаты
   слово 0x0A (смещение картинки) — 0, байт 8 — номер псевдокомнаты, её файл — картинка
   и маска с начала (room.c, gfx_v2.c).
3. Конец игры: «Press ESCAPE to ...» -> «Press KT to ...» (18.LFL, 31.LFL).
4. Титры (скрипт 111 в 49.LFL): страница «Ported to BK 0011M by / Prcoder».
5. Код визы в аэропорту (скрипт 15 в 52.LFL): подходит любой.

Смещения в заголовке комнаты, длина блока и смещения ресурсов файла в индексе
00.LFL сдвигаются. Файлы — XOR 0xFF; дата копии — как у исходного.
"""
import hashlib, os, struct, sys

SRC, DST = sys.argv[1], sys.argv[2]
PAGE_DATA = 0o60000                  # окно страницы СМК (smk.h PAGE_DATA_SIZE)
FILE_MAX = 0o70000 - 0o3200          # буфер загрузчика (zx0lfl.py BUF_SIZE)
PSEUDO = [59, 60]                    # свободные номера комнат (NUM_ROOMS 61)


def fail(msg):
    sys.exit('lfl_patch: ' + msg)


def load(n):
    p = os.path.join(SRC, '%02d.LFL' % n)
    return bytearray(b ^ 0xff for b in open(p, 'rb').read()) if os.path.exists(p) else None


def save(n, d, like):
    path = os.path.join(DST, '%02d.LFL' % n)
    open(path, 'wb').write(bytes(b ^ 0xff for b in d))
    st = os.stat(os.path.join(SRC, '%02d.LFL' % like))
    os.utime(path, (st.st_atime, st.st_mtime))   # mkandos пишет дату файла в каталог


# --- индекс: resource_v2.cpp readEnhancedIndexFile ---
idx = load(0)
if struct.unpack_from('<H', idx, 0)[0] != 0x0100:
    fail('00.LFL — не SCUMM v2')
p = 2
p += 2 + struct.unpack_from('<H', idx, p)[0]
p += 1 + 3 * idx[p]
lists = []   # (начало номеров комнат, начало смещений, число)
for _ in range(3):
    n = idx[p]
    lists.append((p + 1, p + 1 + n, n))
    p += 1 + 3 * n
for rooms, offs, cnt in lists:
    for i in range(cnt):
        if idx[rooms + i] in PSEUDO and struct.unpack_from('<H', idx, offs + 2 * i)[0] != 0xFFFF:
            fail('ресурс в псевдокомнате %d' % idx[rooms + i])


def shift_index(room, start, delta):
    """ресурсы файла room со смещением >= start — на delta"""
    for rooms, offs, cnt in lists:
        for i in range(cnt):
            o = struct.unpack_from('<H', idx, offs + 2 * i)[0]
            if idx[rooms + i] == room and o != 0xFFFF and o >= start:
                struct.pack_into('<H', idx, offs + 2 * i, o + delta)


def cut(f, room, at, end, new):
    """заменить f[at:end] блока комнаты на new: смещения в заголовке, длина, индекс"""
    delta = len(new) - (end - at)
    nobj = f[20]
    words = [0x0C, 0x18, 0x1A] + [28 + 2 * i for i in range(2 * nobj)]
    for w in words:
        o = struct.unpack_from('<H', f, w)[0]
        if o >= end:
            struct.pack_into('<H', f, w, o + delta)
        elif at < o:
            fail('комната %d: смещение %d внутри вырезаемого' % (room, o))
    size = struct.unpack_from('<H', f, 0)[0]
    struct.pack_into('<H', f, 0, size + delta)
    shift_index(room, size, delta)
    f[at:end] = new


# --- картинка: GdiV2 (tools/v2gfx.py decode) ---
def decode(src, p, w, h):
    """-> (точки по колонкам, смещение конца картинки, конца маски)"""
    px = bytearray(w * h)
    dt = [0] * h
    run, color, dither = 1, 0, False
    for x in range(w):
        for y in range(h):
            run -= 1
            if run == 0:
                data = src[p]
                p += 1
                if data & 0x80:
                    run, dither = data & 0x7F, True
                else:
                    run, dither = data >> 4, False
                color = data & 0x0F
                if run == 0:
                    run = src[p]
                    p += 1
            if not dither:
                dt[y] = color
            px[x * h + y] = dt[y]
    pic_end = p
    ns, n = w // 8, 0   # маска: серии до w/8 * h байт
    while n < ns * h:
        run = src[p]
        p += 1
        if run & 0x80:
            p += 1
            n += run & 0x7F
        else:
            p += run
            n += run
    if n != ns * h:
        fail('серия маски за краем')
    return px, pic_end, p


def encode(px, w, h):
    """кратчайший RLE v2 тех же точек: цвет (1..7 — байт, до 255 — два), повтор колонки
    слева (до 127 — байт, до 255 — два); повтор — не в первой колонке полосы"""
    n = w * h
    cp = bytearray(n)
    for i in range(h, n):
        cp[i] = (i // h) % 8 != 0 and px[i] == px[i - h]
    inf = 1 << 30
    dp = [inf] * (n + 1)
    dp[0] = 0
    ch = [None] * (n + 1)
    for i in range(n):
        c0 = dp[i]
        c = px[i]
        k = 0
        while i + k < n and k < 255 and px[i + k] == c:
            k += 1
            cost = c0 + (1 if k <= 7 else 2)
            if cost < dp[i + k]:
                dp[i + k] = cost
                ch[i + k] = (0, k, c)
        k = 0
        while i + k < n and k < 255 and cp[i + k]:
            k += 1
            cost = c0 + (1 if k <= 127 else 2)
            if cost < dp[i + k]:
                dp[i + k] = cost
                ch[i + k] = (1, k, 0)
    out = []
    i = n
    while i:
        t, k, c = ch[i]
        i -= k
        if t == 0:
            out.append(bytes([k << 4 | c]) if k <= 7 else bytes([c, k]))
        else:
            out.append(bytes([0x80 | k]) if k <= 127 else bytes([0x80, k]))
    return b''.join(reversed(out))


# --- титры (скрипт 111 в 49.LFL): страницы — глаголы 1..3 (verbOps 7A: глагол, x, y,
# слот, предлог 0, текст), cursorCommand(0) показывает их, затем ожидание такта музыки.
# Перед последним «delete 2» — своя страница: «Ported to BK 0011M by» / «Prcoder» на
# 4 с (delay, как в MM) ---
CREDITS_ROOM, CREDITS_SCRIPT = 49, 111
LAST = b'Project Directed by David Fox\x00\x60\x00\x00\x80\xa8\x3c\xfb\xff\x9a\x3c\xea'
PAGE = (b'\x7a\x01\x09\x13\x01\x00Ported to BK 0011M by\x00'
        b'\x7a\x02\x10\x14\x02\x00Prcoder\x00'
        b'\x60\x00\x00\x2e\x0f\xff\xff\x7a\x00\x01')


def credits(f):
    rooms, offs, cnt = lists[1]
    if idx[rooms + CREDITS_SCRIPT] != CREDITS_ROOM:
        fail('скрипт %d не в %02d.LFL' % (CREDITS_SCRIPT, CREDITS_ROOM))
    off = struct.unpack_from('<H', idx, offs + 2 * CREDITS_SCRIPT)[0]
    if struct.unpack_from('<H', f, 0)[0] > off:
        fail('скрипт %d — внутри блока комнаты' % CREDITS_SCRIPT)
    size = struct.unpack_from('<H', f, off)[0]
    body = bytes(f[off:off + size])
    at = body.find(LAST)
    if at < 0 or body.count(LAST) != 1:
        fail('%02d.LFL: нет титров для правки' % CREDITS_ROOM)
    at += len(LAST)
    struct.pack_into('<H', f, off, size + len(PAGE))
    f[off + at:off + at] = PAGE
    shift_index(CREDITS_ROOM, off + 1, len(PAGE))


# --- защита: код визы. Скрипт 15 (52.LFL) читает цифры с клавиатуры комнаты 52 и сверяет
# с V139+k; неверная — бит 1467. Режим V119 = 3 — аэропорт (скрипт 30), 1 — телефон
# (головоломка, остаётся). Перед отметкой бита — «isNotEqual V119 3»: в аэропорту отметка
# пропускается, подходит любой код. Переходы через вставку — по смещениям кода скрипта ---
VISA_ROOM, VISA_SCRIPT, VISA_SHA1 = 52, 15, '7115cbe981b98333'
VISA_AT = 187                                   # setBitVar 1467, 0, V235
VISA_INS = b'\x08\x77\x03\x00\x05\x00'          # isNotEqual V119, 3 -> +5
VISA_JUMPS = [(173, 192), (185, 192), (198, 131), (270, 8)]   # слово перехода, цель


def visa(f):
    rooms, offs, cnt = lists[1]
    if idx[rooms + VISA_SCRIPT] != VISA_ROOM:
        fail('скрипт %d не в %02d.LFL' % (VISA_SCRIPT, VISA_ROOM))
    off = struct.unpack_from('<H', idx, offs + 2 * VISA_SCRIPT)[0]
    size = struct.unpack_from('<H', f, off)[0]
    if hashlib.sha1(f[off:off + size]).hexdigest()[:16] != VISA_SHA1:
        fail('%02d.LFL: скрипт %d не тот' % (VISA_ROOM, VISA_SCRIPT))
    code = off + 4
    n = len(VISA_INS)
    for w, t in VISA_JUMPS:
        mv = lambda a: a + n if a > VISA_AT else a
        struct.pack_into('<h', f, code + w, mv(t) - mv(w + 2))
    struct.pack_into('<H', f, off, size + n)
    f[code + VISA_AT:code + VISA_AT] = VISA_INS
    shift_index(VISA_ROOM, off + 1, n)


os.makedirs(DST, exist_ok=True)
pseudo = list(PSEUDO)
for r in range(1, 61):
    f = load(r)
    if f is None:
        continue
    w, h = struct.unpack_from('<HH', f, 4)
    im = struct.unpack_from('<H', f, 0x0A)[0]
    if struct.unpack_from('<H', f, 8)[0] != 0:
        fail('комната %d: байт 8 заголовка занят' % r)
    px, pend, mend = decode(f, im, w, h)
    new = encode(px, w, h)
    if decode(new + f[pend:mend], 0, w, h)[0] != px:
        fail('комната %d: перекодированная картинка не совпадает' % r)
    if len(new) < pend - im:
        cut(f, r, im, pend, new)
    size = struct.unpack_from('<H', f, 0)[0]
    if size > PAGE_DATA or len(f) > FILE_MAX:
        if not pseudo:
            fail('комната %d: нет свободной псевдокомнаты' % r)
        pr = pseudo.pop(0)
        mend = decode(f, im, w, h)[2]
        bg = bytes(f[im:mend])
        struct.pack_into('<HH', f, 0x0A, 0, 0)
        cut(f, r, im, mend, b'')
        f[8] = pr
        save(pr, bytearray(bg), r)
        print('lfl_patch: картинка комнаты %d (%d байт) — в %02d.LFL' % (r, len(bg), pr))
    size = struct.unpack_from('<H', f, 0)[0]
    if size > PAGE_DATA or len(f) > FILE_MAX:
        fail('комната %d: блок %d, файл %d — не помещается' % (r, size, len(f)))
    if r == CREDITS_ROOM:
        credits(f)
    if r == VISA_ROOM:
        visa(f)
    if r == 18 or r == 31:
        old = b'\xa0\xa0\xa0Pres\xf3ESCAP\xc5t\xef'
        rep = b'\xa0\xa0\xa0\xa0Pres\xf3K\xd4t\xef'   # «Press KT to», ещё 2 пробела в начале
        at = f.find(old)
        if at < 0 or f.count(old) != 1:
            fail('%02d.LFL: нет «Press ESCAPE»' % r)
        end = f.index(b'again.\x00', at) + 6
        f[at:end] = rep + f[at + len(old):end] + b' ' * (len(old) - len(rep))
    save(r, f, r)
save(0, idx, 0)
