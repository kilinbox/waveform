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

#include "source.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>

// portable non-SIMD implementation
// see comments of WAVSourceAVX2
void WAVSourceGeneric::tick_spectrum(float seconds)
{
    //std::lock_guard lock(m_mtx); // now locked in tick()
    if(!check_audio_capture(seconds))
        return;

    if(m_capture_channels == 0)
        return;

    const auto bufsz = m_fft_size * sizeof(float);
    const auto outsz = m_fft_size / 2;
    constexpr auto step = 1;

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
        for(auto i = 0u; i < m_fft_size; i += step)
        {
            if(m_fft_input[i] != 0.0f)
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
            auto floor = (float)(m_floor - 10);
            for(size_t i = 0; i < outsz; i += step)
            {
                const auto ch = (m_stereo) ? channel : 0u;
                if(m_decibels[ch][i] > floor)
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
                inbuf[i] *= mulbuf[i];
        }

        if(m_fft_plan != nullptr)
            fftwf_execute(m_fft_plan);
        else
            continue;

        const auto mag_coefficient = 2.0f / (float)m_fft_size;
        const auto g = m_gravity;
        const auto g2 = 1.0f - g;
        const bool slope = m_slope > 0.0f;
        for(size_t i = 0; i < outsz; i += step)
        {
            auto real = m_fft_output[i][0];
            auto imag = m_fft_output[i][1];

            auto mag = std::sqrt((real * real) + (imag * imag)) * mag_coefficient;

            if(slope)
                mag *= m_slope_modifiers[i];

            if(m_tsmoothing == TSmoothingMode::EXPONENTIAL)
            {
                if(m_fast_peaks)
                    m_tsmooth_buf[channel][i] = std::max(mag, m_tsmooth_buf[channel][i]);

                mag = (g * m_tsmooth_buf[channel][i]) + (g2 * mag);
                m_tsmooth_buf[channel][i] = mag;
            }

            m_decibels[channel][i] = mag;
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

void WAVSourceGeneric::tick_meter(float seconds)
{
    if(!check_audio_capture(seconds))
        return;

    if(m_capture_channels == 0)
        return;

    const auto outsz = m_fft_size;

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
        if(m_meter_rms)
        {
            for(size_t i = 0; i < outsz; ++i)
            {
                auto val = m_decibels[channel][i];
                out += val * val;
            }
            out = std::sqrt(out / m_fft_size);
        }
        else
        {
            for(size_t i = 0; i < outsz; ++i)
                out = std::max(out, std::abs(m_decibels[channel][i]));
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
