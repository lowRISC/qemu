/*
 * QEMU OpenTitan 32-bit FIFO helper
 *
 * Copyright (c) 2023 Rivos, Inc.
 * Based on fifo8.h
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_OPENTITAN_OT_FIFO32_H
#define HW_OPENTITAN_OT_FIFO32_H

typedef struct {
    uint32_t *data;
    uint32_t capacity;
    uint32_t head;
    uint32_t num;
} OtFifo32;

static inline void ot_fifo32_create(OtFifo32 *fifo, uint32_t capacity)
{
    fifo->data = g_new(uint32_t, capacity);
    fifo->capacity = capacity;
    fifo->head = 0u;
    fifo->num = 0u;
}

static inline void ot_fifo32_push(OtFifo32 *fifo, uint32_t data)
{
    g_assert(fifo->num < fifo->capacity);

    fifo->data[(fifo->head + fifo->num) % fifo->capacity] = data;
    fifo->num++;
}

static inline uint32_t ot_fifo32_pop(OtFifo32 *fifo)
{
    g_assert(fifo->num > 0);

    uint32_t ret = fifo->data[fifo->head];

    fifo->head++;
    fifo->head %= fifo->capacity;
    fifo->num--;

    return ret;
}

static inline uint32_t ot_fifo32_peek(OtFifo32 *fifo)
{
    g_assert(fifo->num > 0);

    uint32_t ret = fifo->data[fifo->head];

    return ret;
}

static inline const uint32_t *
ot_fifo32_pop_buf(OtFifo32 *fifo, uint32_t max, uint32_t *num)
{
    uint32_t *ret;

    g_assert(max > 0 && max <= fifo->num);
    *num = MIN(fifo->capacity - fifo->head, max);
    ret = &fifo->data[fifo->head];
    fifo->head += *num;
    fifo->head %= fifo->capacity;
    fifo->num -= *num;
    return ret;
}

static inline const uint32_t *
ot_fifo32_peek_buf(OtFifo32 *fifo, uint32_t max, uint32_t *num)
{
    uint32_t *ret;

    g_assert(max > 0 && max <= fifo->num);
    *num = MIN(fifo->capacity - fifo->head, max);
    ret = &fifo->data[fifo->head];
    return ret;
}

static inline void ot_fifo32_consume_all(OtFifo32 *fifo, uint32_t num)
{
    num = MIN(fifo->capacity - fifo->head, num);
    fifo->head += num;
    fifo->head %= fifo->capacity;
    fifo->num -= num;
}

static inline void ot_fifo32_reset(OtFifo32 *fifo)
{
    fifo->num = 0u;
    fifo->head = 0u;
}

static inline bool ot_fifo32_is_empty(OtFifo32 *fifo)
{
    return (fifo->num == 0u);
}

static inline bool ot_fifo32_is_full(OtFifo32 *fifo)
{
    return (fifo->num == fifo->capacity);
}

static inline uint32_t ot_fifo32_num_free(OtFifo32 *fifo)
{
    return fifo->capacity - fifo->num;
}

static inline uint32_t ot_fifo32_num_used(OtFifo32 *fifo)
{
    return fifo->num;
}

#endif /* HW_OPENTITAN_OT_FIFO32_H */
