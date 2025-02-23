/*
    Copyright (C) 2022 Devin Davila

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "waveform_config.hpp"
#include "source.hpp"
#include <immintrin.h>
#include <algorithm>
#include <cstring>

// adaptation of WAVSourceAVX2 to support CPUs without AVX2
// see comments of WAVSourceAVX2
DECORATE_AVX
void WAVSourceAVX::tick_spectrum(float seconds)
{
    //std::lock_guard lock(m_mtx); // now locked in tick()
    if(!check_audio_capture(seconds))
        return;

    if(m_capture_channels == 0)
        return;

    const auto bufsz = m_fft_size * sizeof(float);
    const auto outsz = m_fft_size / 2;
    constexpr auto step = sizeof(__m256) / sizeof(float);

    if(!m_show)
    {
        if(m_last_silent)
            return;
        for(auto channel = 0u; channel < m_capture_channels; ++channel)
            if(m_tsmooth_buf[channel] != nullptr)
                memset(m_tsmooth_buf[channel].get(), 0, outsz * sizeof(float));
        for(auto channel = 0; channel < (m_stereo ? 2 : 1); ++channel)
            for(size_t i = 0; i < outsz; ++i)
                m_decibels[channel][i] = DB_MIN;
        m_last_silent = true;
        return;
    }

    auto silent_channels = 0u;
    for(auto channel = 0u; channel < m_capture_channels; ++channel)
    {
        if(m_capturebufs[channel].size >= bufsz)
        {
            circlebuf_peek_front(&m_capturebufs[channel], m_fft_input.get(), bufsz);
            circlebuf_pop_front(&m_capturebufs[channel], nullptr, m_capturebufs[channel].size - bufsz);
        }
        else
            continue;

        bool silent = true;
        const auto zero = _mm256_setzero_ps();
        for(auto i = 0u; i < m_fft_size; i += step)
        {
            auto mask = _mm256_cmp_ps(zero, _mm256_load_ps(&m_fft_input[i]), _CMP_EQ_OQ);
            if(_mm256_movemask_ps(mask) != 0xff)
            {
                silent = false;
                m_last_silent = false;
                break;
            }
        }

        if(silent)
        {
            if(m_last_silent)
                continue;
            bool outsilent = true;
            auto floor = _mm256_set1_ps((float)(m_floor - 10));
            for(size_t i = 0; i < outsz; i += step)
            {
                const auto ch = (m_stereo) ? channel : 0u;
                auto mask = _mm256_cmp_ps(floor, _mm256_load_ps(&m_decibels[ch][i]), _CMP_GT_OQ);
                if(_mm256_movemask_ps(mask) != 0xff)
                {
                    outsilent = false;
                    break;
                }
            }
            if(outsilent)
            {
                if(++silent_channels >= m_capture_channels)
                    m_last_silent = true;
                continue;
            }
        }

        if(m_window_func != FFTWindow::NONE)
        {
            auto inbuf = m_fft_input.get();
            auto mulbuf = m_window_coefficients.get();
            for(auto i = 0u; i < m_fft_size; i += step)
                _mm256_store_ps(&inbuf[i], _mm256_mul_ps(_mm256_load_ps(&inbuf[i]), _mm256_load_ps(&mulbuf[i])));
        }

        if(m_fft_plan != nullptr)
            fftwf_execute(m_fft_plan);
        else
            continue;

        constexpr auto shuffle_mask_r = 0 | (2 << 2) | (0 << 4) | (2 << 6);
        constexpr auto shuffle_mask_i = 1 | (3 << 2) | (1 << 4) | (3 << 6);
        const auto mag_coefficient = _mm256_div_ps(_mm256_set1_ps(2.0f), _mm256_set1_ps((float)m_fft_size));
        const auto g = _mm256_set1_ps(m_gravity);
        const auto g2 = _mm256_sub_ps(_mm256_set1_ps(1.0), g);
        const bool slope = m_slope > 0.0f;
        for(size_t i = 0; i < outsz; i += step)
        {
            // load 8 real/imaginary pairs and group the r/i components in the low/high halves
            // de-interleaving 256-bit float vectors is nigh impossible without AVX2, so we'll
            // use 128-bit vectors and merge them, but i question if this is better than a 128-bit loop
            const float *buf = &m_fft_output[i][0];
            auto chunk1 = _mm_load_ps(buf);
            auto chunk2 = _mm_load_ps(&buf[4]);
            auto rvec = _mm256_castps128_ps256(_mm_shuffle_ps(chunk1, chunk2, shuffle_mask_r)); // group octwords
            auto ivec = _mm256_castps128_ps256(_mm_shuffle_ps(chunk1, chunk2, shuffle_mask_i));
            chunk1 = _mm_load_ps(&buf[8]);
            chunk2 = _mm_load_ps(&buf[12]);
            rvec = _mm256_insertf128_ps(rvec, _mm_shuffle_ps(chunk1, chunk2, shuffle_mask_r), 1); // pack r/i octwords into separate 256-bit vecs
            ivec = _mm256_insertf128_ps(ivec, _mm_shuffle_ps(chunk1, chunk2, shuffle_mask_i), 1);

            auto mag = _mm256_sqrt_ps(_mm256_fmadd_ps(ivec, ivec, _mm256_mul_ps(rvec, rvec)));
            mag = _mm256_mul_ps(mag, mag_coefficient);

            if(slope)
                mag = _mm256_mul_ps(mag, _mm256_load_ps(&m_slope_modifiers[i]));

            if(m_tsmoothing == TSmoothingMode::EXPONENTIAL)
            {
                if(m_fast_peaks)
                    _mm256_store_ps(&m_tsmooth_buf[channel][i], _mm256_max_ps(mag, _mm256_load_ps(&m_tsmooth_buf[channel][i])));

                mag = _mm256_fmadd_ps(g, _mm256_load_ps(&m_tsmooth_buf[channel][i]), _mm256_mul_ps(g2, mag));
                _mm256_store_ps(&m_tsmooth_buf[channel][i], mag);
            }

            _mm256_store_ps(&m_decibels[channel][i], mag);
        }
    }

    if(m_last_silent)
        return;

    if(m_output_channels > m_capture_channels)
        memcpy(m_decibels[1].get(), m_decibels[0].get(), outsz * sizeof(float));

    if(m_stereo)
    {
        for(auto channel = 0; channel < 2; ++channel)
            for(size_t i = 0; i < outsz; ++i)
                m_decibels[channel][i] = dbfs(m_decibels[channel][i]);
    }
    else if(m_capture_channels > 1)
    {
        for(size_t i = 0; i < outsz; ++i)
            m_decibels[0][i] = dbfs((m_decibels[0][i] + m_decibels[1][i]) * 0.5f);
    }
    else
    {
        for(size_t i = 0; i < outsz; ++i)
            m_decibels[0][i] = dbfs(m_decibels[0][i]);
    }
}

DECORATE_AVX
void WAVSourceAVX::tick_meter(float seconds)
{
    if(!check_audio_capture(seconds))
        return;

    if(m_capture_channels == 0)
        return;

    // repurpose m_decibels as circular buffer for sample data
    for(auto channel = 0u; channel < m_capture_channels; ++channel)
    {
        while(m_capturebufs[channel].size > 0)
        {
            auto consume = m_capturebufs[channel].size;
            auto max = (m_fft_size - m_meter_pos[channel]) * sizeof(float);
            if(consume >= max)
            {
                circlebuf_pop_front(&m_capturebufs[channel], &m_decibels[channel][m_meter_pos[channel]], max);
                m_meter_pos[channel] = 0;
            }
            else
            {
                circlebuf_pop_front(&m_capturebufs[channel], &m_decibels[channel][m_meter_pos[channel]], consume);
                m_meter_pos[channel] += consume / sizeof(float);
            }
        }
    }

    if(!m_show)
        return;

    for(auto channel = 0u; channel < m_capture_channels; ++channel)
    {
        float out = 0.0f;
        constexpr auto step = (sizeof(__m256) / sizeof(float)) * 2; // buffer size is 64-byte multiple
        constexpr auto halfstep = step / 2;
        if(m_meter_rms)
        {
            auto sum = _mm256_setzero_ps();
            for(size_t i = 0; i < m_fft_size; i += step)
            {
                auto chunk = _mm256_load_ps(&m_decibels[channel][i]);
                sum = _mm256_fmadd_ps(chunk, chunk, sum);
                chunk = _mm256_load_ps(&m_decibels[channel][i + halfstep]); // unroll loop to cache line size
                sum = _mm256_fmadd_ps(chunk, chunk, sum);
            }

            auto high = _mm256_extractf128_ps(sum, 1); // split into two 128-bit vecs
            auto low = _mm_add_ps(high, _mm256_castps256_ps128(sum)); // (h[0] + l[0]) (h[1] + l[1]) (h[2] + l[2]) (h[3] + l[3])
            high = _mm_permute_ps(low, _MM_SHUFFLE(3, 2, 3, 2)); // high[0] = low[2], high[1] = low[3]
            low = _mm_add_ps(high, low); // (h[0] + l[0]) (h[1] + l[1])
            high = _mm_movehdup_ps(low); // high[0] = low[1]
            out = _mm_cvtss_f32(_mm_add_ss(high, low));

            out = std::sqrt(out / m_fft_size);
        }
        else
        {
            const auto signbit = _mm256_set1_ps(-0.0f);
            auto maxvec = _mm256_setzero_ps();
            for(size_t i = 0; i < m_fft_size; i += step)
            {
                auto chunk = _mm256_andnot_ps(signbit, _mm256_load_ps(&m_decibels[channel][i])); // absolute value
                maxvec = _mm256_max_ps(maxvec, chunk);
                chunk = _mm256_andnot_ps(signbit, _mm256_load_ps(&m_decibels[channel][i + halfstep])); // unroll loop to cache line size
                maxvec = _mm256_max_ps(maxvec, chunk);
            }

            auto high = _mm256_extractf128_ps(maxvec, 1); // split into two 128-bit vecs
            auto low = _mm_max_ps(high, _mm256_castps256_ps128(maxvec)); // max(h[0], l[0]) max(h[1], l[1]) max(h[2], l[2]) max(h[3], l[3])
            high = _mm_permute_ps(low, _MM_SHUFFLE(3, 2, 3, 2)); // high[0] = low[2], high[1] = low[3]
            low = _mm_max_ps(high, low); // max(h[0], l[0]) max(h[1], l[1])
            high = _mm_movehdup_ps(low); // high[0] = low[1]
            out = _mm_cvtss_f32(_mm_max_ss(high, low));
        }

        if(m_tsmoothing == TSmoothingMode::EXPONENTIAL)
        {
            const auto g = m_gravity;
            const auto g2 = 1.0f - g;
            if(!m_fast_peaks || (out <= m_meter_buf[channel]))
                out = (g * m_meter_buf[channel]) + (g2 * out);
        }
        m_meter_buf[channel] = out;
        m_meter_val[channel] = dbfs(out);
    }
}
