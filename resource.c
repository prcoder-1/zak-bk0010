/* Ресурсы: индекс 00.LFL и кэш в страницах СМК.
 * resource_v2.cpp: readIndexFile / readEnhancedIndexFile / readGlobalObjects,
 * resource.cpp: ensureResourceLoaded / loadResource / getResourceAddress.
 *
 * В ScummVM ресурс читается из файла комнаты по (комната, смещение) в кучу. Здесь
 * файл NN.LFL читается целиком (EMT 36 ANDOS иначе не умеет) в страницу кэша:
 * блок комнаты лежит с её начала. Все глобальные ресурсы этого файла (скрипты,
 * костюмы, звуки) сразу переносятся в кучу и больше не вытесняются — повторное
 * чтение файла стоит секунды. Страницы комнат вытесняются по давности (LRU). */
#include "scumm.h"
#include "con.h"
#include "az.h"

#define RES_NONE 0      /* ещё не загружен */
#define RES_BAD  0377   /* битая запись индекса: смещение или длина вне файла */

static uint8_t num_res[3];                     /* костюмы, скрипты, звуки */
static uint8_t room_of[3][NUM_SCRIPTS];        /* файл (комната) ресурса */
static uint16_t offs_of[3][NUM_SCRIPTS];       /* смещение в файле, 0xFFFF — нет */
static uint8_t cpage[3][NUM_SCRIPTS];          /* где лежит: страница / RES_* */
static uint16_t caddr[3][NUM_SCRIPTS];

static uint8_t file_page[NUM_ROOMS];           /* страница файла комнаты, 0 — нет */
static uint16_t file_len[NUM_ROOMS];

/* страницы кэша PG_CACHE..PG_LAST: 0 свободна, 0377 куча, иначе комната+1 */
#define PG_FREE 0
#define PG_HEAP 0377
static uint8_t pg_use[16];
static uint16_t pg_stamp[16];
static uint16_t stamp;
static uint8_t locked_room, locked_bg;
static uint8_t busy_page;   /* файл, который сейчас разбирается */

static uint8_t tix(uint8_t type)
{
    if (type == rtCostume) return 0;
    if (type == rtScript) return 1;
    return 2;
}

uint8_t res_num(uint8_t type)
{
    return num_res[tix(type)];
}

/* resource_v2.cpp: readEnhancedIndexFile + readResTypeList(v3old) + readGlobalObjects.
 * 00.LFL кладётся во временную страницу и разбирается словами через far_*. */
static uint16_t ip_page, ip;

static uint8_t ib(void)
{
    return far_rb(ip_page, ip++);
}

static uint16_t iw(void)
{
    uint16_t v = far_rw(ip_page, ip);
    ip += 2;
    return v;
}

static void read_list(uint8_t t, uint8_t n)
{
    if (n > NUM_SCRIPTS) fatal("INDEX", n);
    num_res[t] = n;
    for (uint8_t i = 0; i < n; ++i) room_of[t][i] = ib();
    for (uint8_t i = 0; i < n; ++i) offs_of[t][i] = iw();
}

static uint8_t take_page(void);

/* чтение файла. Драйвером ПЗУ (файл подряд, таблицы ftab.py) буфер загрузчика
 * (03200..067777) в окне 040000 — в скрытой странице 3 ОЗУ 11М: экран как был.
 * Через ДОС буфер перекрывает экран, видна лишь его нижняя четверть (070000..077777,
 * «малый экран») — там надпись; после чтения экран чистый и перерисовывается */
#ifndef HOST
/* надпись LOADING и имя файла — в строке 29 видимой страницы, ниже 200 строк игры (как в
 * v1); стирается в конце шага (ld_label_clear) на той же странице */
static void ld_label(const char *name)
{
    if (az_on)
    {
        azc(AZ_LABEL, name);
        ld_label_pg = 1;
        return;
    }
    cursor_hide();
    uint16_t m = mem11;
    w1_map(scr_shown);   /* видимая: окно 040000 может смотреть в скрытую (вывод вида) */
    con_puts(29, 9, "LOADING", 2);
    con_puts(29, 17, name, 2);
    mem11 = m;   /* тень — первой: кадровое прерывание возвращает окно по ней */
    *(volatile uint16_t *)0177716 = m;
    ld_label_pg = scr_shown;
}

void ld_label_clear(void)
{
    if (az_on)
    {
        azc(AZ_LABEL, 0);
        ld_label_pg = 0;
        return;
    }
    cursor_hide();
    uint16_t m = mem11;
    w1_map(ld_label_pg);
    volatile uint16_t *v = (volatile uint16_t *)(040000u + 29 * 8 * 64);
    for (uint16_t n = 8 * 64 / 2; n; --n) *v++ = 0;
    mem11 = m;
    *(volatile uint16_t *)0177716 = m;
    ld_label_pg = 0;
}
#endif

static int16_t ld_screen(const struct ld_req *q)
{
#ifndef HOST
    if (q->fblk || az_on) ld_label(q->name);   /* драйвер ПЗУ (и AZБК): экран цел, надпись — до конца шага */
    if (!q->fblk && !az_on)   /* через ДОС: виден лишь «малый экран» 070000..077777 */
    {
        volatile uint16_t *v = (volatile uint16_t *)070000;
        for (uint16_t n = 04000; n; --n) *v++ = 0;
        con_puts(27, 12, "LOADING", 2);
        con_puts(29, 12, q->name, 2);
    }
#endif
    int16_t len = ld_file(q);
#ifndef HOST
    if (ld_fast || az_on) return len;   /* драйвер ПЗУ читал в скрытую страницу, у AZБК экран свой: цел */
    gfx_clear_all();
    con_puts(13, 12, "LOADING", 2);   /* до перерисовки (декодирование комнаты — секунды) */
    con_puts(15, 12, q->name, 2);
#endif
    screen_trashed = 1;
    return len;
}

/* блок, длина, первое слово NN.LFL на диске — пишет tools/ftab.py в образ (0 —
 * через ДОС) */
uint16_t lfl_ft[NUM_ROOMS][3] = { { 0, 0, 1 } };   /* не нули: в .data (в файле) */

void res_init(void)
{
    ip_page = take_page();
    struct ld_req q = { "00.LFL", LD_XOR | LD_ZX0, ip_page, PAGE_ADDR, PAGE_DATA_SIZE, PG_MASK, OVF_ADDR,
                        lfl_ft[0][0], lfl_ft[0][1], lfl_ft[0][2] };
    if (ld_screen(&q) < 0) fatal("00.LFL", ld_resp);
    ip = PAGE_ADDR;
    if (iw() != 0x0100) fatal("NOT ZAK V2", 0);
    uint16_t n = iw();
    if (n > NUM_GLOBAL_OBJECTS) fatal("OBJECTS", n);
    numGlobalObjects = n;
    for (uint16_t i = 0; i < n; ++i)
    {
        uint8_t b = ib();
        objectOwnerTable[i] = b & 0x0F;   /* OF_OWNER_MASK */
        objectStateTable[i] = b >> 4;     /* OF_STATE_SHL */
    }
    uint8_t nr = ib();                    /* комнаты: номер = индекс, смещения не нужны */
    if (nr > NUM_ROOMS) fatal("ROOMS", nr);
    ip += (uint16_t)nr * 3;
    read_list(0, ib());
    read_list(1, ib());
    read_list(2, ib());
    pg_use[ip_page] = PG_FREE;
}

/* --- страницы кэша --- */

/* свободная или давно не нужная страница файла комнаты; 0 — нет */
static uint8_t find_page(void)
{
    uint8_t best = 0;
    for (uint8_t p = PG_CACHE; p <= PG_LAST; ++p)
    {
        if (pg_use[p] == PG_FREE) return p;
        if (pg_use[p] == PG_HEAP) continue;
        if (pg_use[p] == (uint8_t)(locked_room + 1) || p == busy_page) continue;
        if (locked_bg && pg_use[p] == (uint8_t)(locked_bg + 1)) continue;
        if (!best || (uint16_t)(stamp - pg_stamp[p]) > (uint16_t)(stamp - pg_stamp[best])) best = p;
    }
    return best;
}

static uint8_t take_page(void)
{
    uint8_t best = find_page();
    if (!best) fatal("NO PAGES", 0);
    if (pg_use[best] != PG_FREE)
    {
        uint8_t room = (uint8_t)(pg_use[best] - 1);   /* вытесняется файл комнаты */
        file_page[room] = 0;
    }
    pg_use[best] = 0376;   /* занята (уточнит вызывающий) */
    pg_stamp[best] = ++stamp;
    return best;
}

/* --- куча: блоки [слово: длина с заголовком | 1 — занят][данные] --- */

/* куча — не больше HEAP_PAGES страниц: остальные — файлы комнат (текущая, её картинка,
 * читаемый файл). Глобальных ресурсов у Zak ~150 Кбайт — нужные сейчас помещаются, прочие
 * вытесняются (res_evict) и читаются из файла заново */
#ifndef HEAP_PAGES
#define HEAP_PAGES 4
#endif
static uint8_t heap_pages;
static uint8_t keep_t = 0377, keep_i;   /* ресурс, ради которого читается файл */

static void heap_page(uint8_t p)
{
    pg_use[p] = PG_HEAP;
    heap_pages++;
    uint16_t h = PAGE_DATA_SIZE;
    far_wr(p, PAGE_ADDR, &h, 2);
}

/* свободный блок need байт (со слиянием соседних свободных); страница или 0 */
static uint8_t heap_find(uint16_t need)
{
    for (uint8_t p = PG_CACHE; p <= PG_LAST; ++p)
    {
        if (pg_use[p] != PG_HEAP) continue;
        uint16_t a = PAGE_ADDR, end = PAGE_ADDR + PAGE_DATA_SIZE;
        while (a < end)
        {
            uint16_t h = far_rw(p, a);
            uint16_t len = h & ~1u;
            if (!(h & 1))
            {
                /* слить со следующими свободными */
                while (a + len < end)
                {
                    uint16_t h2 = far_rw(p, a + len);
                    if (h2 & 1) break;
                    len += h2;
                }
                if (len >= need)
                {
                    uint16_t w;
                    if (len - need >= 8)
                    {
                        w = len - need;
                        far_wr(p, a + need, &w, 2);
                        len = need;
                    }
                    w = len | 1;
                    far_wr(p, a, &w, 2);
                    res_addr = a + 2;
                    /* блок заполнит вызывающий (скрипт, OBCD предмета): строки кэша
                     * байт-кода могли захватить это место, пока оно было свободным */
                    vm_flush();
                    return p;
                }
                if (len != (h & ~1u)) far_wr(p, a, &len, 2);
            }
            a += len;
        }
    }
    return 0;
}

void res_free(uint16_t page, uint16_t addr)
{
    uint16_t h = far_rw(page, addr - 2) & ~1u;
    far_wr(page, addr - 2, &h, 2);
}

/* ScummVM expireResources: освободить ресурсы типа t, которые сейчас не нужны: звуки
 * (драйвер играет свою копию), скрипты вне слотов, костюмы без актёров в комнате */
static void res_evict(uint8_t t)
{
    uint8_t any = 0;
    for (uint8_t i = 1; i < num_res[t]; ++i)
    {
        uint8_t pg = cpage[t][i];
        if (pg == RES_NONE || pg == RES_BAD || (t == keep_t && i == keep_i)) continue;
        if (t == 1 && isScriptRunning(i)) continue;
        if (t == 0 && cost_cache(i)) continue;
        res_free(pg, caddr[t][i]);
        cpage[t][i] = RES_NONE;
        any = 1;
    }
    if (any && t == 0) cost_cache(0);
    vm_flush();
}

/* блок кучи; evict = 0 — без вытеснения ресурсов (0, если места нет) */
static uint8_t res_alloc_ex(uint16_t size, uint8_t evict)
{
    uint16_t need = (uint16_t)((size + 3) & ~1u);
    for (uint8_t stage = 0;; ++stage)
    {
        uint8_t p = heap_find(need);
        if (p) return p;
        if (heap_pages < HEAP_PAGES && find_page())
        {
            heap_page(take_page());
            continue;
        }
        if (!evict || stage >= 3) break;
        res_evict((uint8_t)(2 - stage));   /* звуки, скрипты, костюмы */
    }
    if (!evict) return 0;
    fatal("HEAP", size);
}

uint16_t res_alloc(uint16_t size)
{
    return res_alloc_ex(size, 1);
}

/* --- файлы комнат --- */

/* перенести ресурс файла (страница pg, хвост сверх 24 Кбайт — в PG_MASK:OVF_ADDR) в кучу */
static void file_copy(uint8_t dpage, uint16_t daddr, uint8_t pg, uint16_t off, uint16_t n)
{
    if (off < PAGE_DATA_SIZE)
    {
        uint16_t k = n;
        if (off + k > PAGE_DATA_SIZE) k = PAGE_DATA_SIZE - off;
        far_cp(dpage, daddr, pg, PAGE_ADDR + off, k);
        daddr += k;
        off += k;
        n -= k;
    }
    if (n) far_cp(dpage, daddr, PG_MASK, OVF_ADDR + (off - PAGE_DATA_SIZE), n);
}

static uint16_t file_word(uint8_t pg, uint16_t off)
{
    if ((uint16_t)(off + 1) < PAGE_DATA_SIZE) return far_rw(pg, PAGE_ADDR + off);
    uint16_t lo = off < PAGE_DATA_SIZE ? far_rb(pg, PAGE_ADDR + off)
                                       : far_rb(PG_MASK, OVF_ADDR + (off - PAGE_DATA_SIZE));
    ++off;
    uint16_t hi = off < PAGE_DATA_SIZE ? far_rb(pg, PAGE_ADDR + off)
                                       : far_rb(PG_MASK, OVF_ADDR + (off - PAGE_DATA_SIZE));
    return (uint16_t)(lo | (hi << 8));
}

/* ресурс (t, i) из файла в странице pg (длина len) — в кучу (loadResource: длина — первое
 * слово); evict = 0 — только если есть место */
static void copy_res(uint8_t t, uint8_t i, uint8_t pg, uint16_t len, uint8_t evict)
{
    uint16_t off = offs_of[t][i];
    uint16_t size = off < len - 1 ? file_word(pg, off) : 0;
    if (off == 0xFFFF || size < 4 || (uint16_t)(off + size) > len || off + size < off)
    {
        cpage[t][i] = RES_BAD;
        return;
    }
    uint8_t hp = res_alloc_ex(size, evict);
    if (!hp) return;   /* места нет: прочтётся, когда понадобится */
    uint16_t a = res_addr;
    file_copy(hp, a, pg, off, size);
    cpage[t][i] = hp;
    caddr[t][i] = a;
}

/* файл комнаты — в страницу кэша; глобальные ресурсы файла — в кучу: (wt, wi) — нужный
 * (wt = 0377 — нет), прочие — пока есть место (хвост файла сверх 24 Кбайт живёт только
 * до конца чтения) */
static void load_file(uint8_t room, uint8_t wt, uint8_t wi)
{
    char name[7] = "00.LFL";
    name[0] = (char)('0' + room / 10);
    name[1] = (char)('0' + room % 10);
    uint8_t pg = take_page();
    struct ld_req q = { name, LD_XOR | LD_ZX0, pg, PAGE_ADDR, PAGE_DATA_SIZE, PG_MASK, OVF_ADDR,
                        lfl_ft[room][0], lfl_ft[room][1], lfl_ft[room][2] };
    int16_t len = ld_screen(&q);
    vm_flush();
    if (len < 0) fatal(name, ld_resp);
    pg_use[pg] = (uint8_t)(room + 1);
    file_page[room] = pg;
    busy_page = pg;
    file_len[room] = (uint16_t)len;
    if (wt != 0377)
    {
        keep_t = wt;
        keep_i = wi;
        copy_res(wt, wi, pg, (uint16_t)len, 1);
    }
    for (uint8_t t = 0; t < 3; ++t)
        for (uint8_t i = 0; i < num_res[t]; ++i)
            if (room_of[t][i] == room && cpage[t][i] == RES_NONE) copy_res(t, i, pg, (uint16_t)len, 0);
    keep_t = 0377;
    busy_page = 0;
}

/* getResourceAddress: страница ресурса (адрес — в *addr) или 0, если ресурса нет */
uint8_t res_get(uint8_t type, uint16_t idx)
{
    if (type == rtRoom)
    {
        if (idx == 0 || idx >= NUM_ROOMS) return 0;
        if (!file_page[idx]) load_file((uint8_t)idx, 0377, 0);
        pg_stamp[file_page[idx]] = ++stamp;
        res_addr = PAGE_ADDR;
        return file_page[idx];
    }
    uint8_t t = tix(type);
    if (idx == 0 || idx >= num_res[t]) return 0;   /* ensureResourceLoaded: idx 0 — нет */
    if (cpage[t][idx] == RES_NONE)
    {
        uint8_t room = room_of[t][idx];
        uint16_t off = offs_of[t][idx];
        if (room == 0 || room >= NUM_ROOMS || off == 0xFFFF)
            cpage[t][idx] = RES_BAD;
        else
        {
            uint8_t pg = file_page[room];
            /* файл в кэше и ресурс не в хвосте — без чтения диска */
            if (pg && off < PAGE_DATA_SIZE - 1 && (uint32_t)off + file_word(pg, off) <= PAGE_DATA_SIZE)
            {
                pg_stamp[pg] = ++stamp;
                keep_t = t;
                keep_i = (uint8_t)idx;
                busy_page = pg;
                copy_res(t, (uint8_t)idx, pg, file_len[room], 1);
                busy_page = 0;
                keep_t = 0377;
            }
            else
                load_file(room, t, (uint8_t)idx);
        }
    }
    if (cpage[t][idx] == RES_BAD || cpage[t][idx] == RES_NONE) return 0;
    res_addr = caddr[t][idx];
    return cpage[t][idx];
}

/* текущая комната не вытесняется, и файл её картинки, если он свой (байт 8 заголовка —
 * псевдокомната, gfx_v2.c room_decode) */
void res_lock_room(uint8_t room)
{
    locked_room = room;
    locked_bg = room && file_page[room] ? far_rb(file_page[room], PAGE_ADDR + 8) : 0;
}
