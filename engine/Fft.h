// Minimal radix-2 complex FFT. No dependencies, so the engine stays portable
// and free of third-party licence terms.
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace mender
{
class Fft
{
public:
    explicit Fft (std::size_t size) : n (size), twiddles (size / 2), reversed (size)
    {
        const double pi = 3.14159265358979323846;

        for (std::size_t i = 0; i < n / 2; ++i)
            twiddles[i] = std::polar (1.0, -2.0 * pi * (double) i / (double) n);

        std::size_t bits = 0;
        while ((std::size_t (1) << bits) < n)
            ++bits;

        for (std::size_t i = 0; i < n; ++i)
        {
            std::size_t r = 0;
            for (std::size_t b = 0; b < bits; ++b)
                if (i & (std::size_t (1) << b))
                    r |= std::size_t (1) << (bits - 1 - b);
            reversed[i] = r;
        }
    }

    std::size_t size() const { return n; }

    void forward (std::vector<std::complex<double>>& data) const { transform (data, false); }

    // Inverse transform, scaled by 1/N.
    void inverse (std::vector<std::complex<double>>& data) const
    {
        transform (data, true);
        const double scale = 1.0 / (double) n;
        for (auto& v : data)
            v *= scale;
    }

private:
    void transform (std::vector<std::complex<double>>& data, bool inverse) const
    {
        for (std::size_t i = 0; i < n; ++i)
            if (reversed[i] > i)
                std::swap (data[i], data[reversed[i]]);

        for (std::size_t len = 2; len <= n; len <<= 1)
        {
            const std::size_t half = len / 2;
            const std::size_t step = n / len;

            for (std::size_t start = 0; start < n; start += len)
            {
                for (std::size_t k = 0; k < half; ++k)
                {
                    auto w = twiddles[k * step];
                    if (inverse)
                        w = std::conj (w);

                    const auto a = data[start + k];
                    const auto b = data[start + k + half] * w;
                    data[start + k] = a + b;
                    data[start + k + half] = a - b;
                }
            }
        }
    }

    std::size_t n;
    std::vector<std::complex<double>> twiddles;
    std::vector<std::size_t> reversed;
};
} // namespace mender
