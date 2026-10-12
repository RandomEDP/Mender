// Tests for spectral paste and band-pass listening. Exit code 0 means all passed.
#include "../engine/SpectralEdit.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
const double pi = 3.14159265358979323846;
int failures = 0;

void check (bool ok, const char* name, double value)
{
    std::printf ("%s  %-62s %9.2f\n", ok ? "PASS" : "FAIL", name, value);
    if (! ok)
        ++failures;
}

double powerDb (const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (auto i = from; i < to; ++i)
    {
        const double d = (double) a[i] - (b.empty() ? 0.0 : (double) b[i]);
        sum += d * d;
    }
    return 10.0 * std::log10 (sum / (double) (to - from) + 1.0e-20);
}

std::vector<float> bandOf (const std::vector<float>& x, double sr, double lo, double hi, int fft)
{
    std::vector<float> out (x.size());
    mender::bandPass (x.data(), out.data(), (std::int64_t) x.size(), sr, lo, hi, fft);
    return out;
}

void pasteTest (double sr, int fft)
{
    std::printf ("\n-- paste, %.0f Hz, FFT %d --\n", sr, fft);
    const auto n = (std::size_t) (sr * 3.0);
    std::mt19937 rng (11);
    std::normal_distribution<double> noise (0.0, 0.01);

    // Bed: a 300 Hz tone (outside the band) plus room noise (everywhere).
    std::vector<float> bed (n);
    for (std::size_t i = 0; i < n; ++i)
        bed[i] = (float) (0.3 * std::sin (2 * pi * 300.0 * (double) i / sr) + noise (rng));

    // Damage: a loud 2-4 kHz noise burst ("cough") at 2.0-2.2 s.
    auto damaged = bed;
    const auto d0 = (std::size_t) (2.0 * sr), d1 = (std::size_t) (2.2 * sr);
    for (auto i = d0; i < d1; ++i)
        damaged[i] += (float) (8.0 * noise (rng) * std::sin (pi * (double) (i - d0) / (double) (d1 - d0)));
    const auto burst = bandOf (damaged, sr, 2000.0, 4000.0, fft);

    // Copy clean room tone from 0.5 s, same length plus context, and paste it over the burst.
    const auto context = mender::pasteContext (fft);
    const auto length = (std::int64_t) (0.24 * sr);
    const auto destStart = (std::int64_t) (1.98 * sr);
    const auto srcStart = (std::int64_t) (0.5 * sr);
    std::vector<float> clip ((std::size_t) (length + 2 * context));
    for (std::size_t i = 0; i < clip.size(); ++i)
        clip[i] = damaged[(std::size_t) (srcStart - context) + i];

    auto result = damaged;
    float* dest[] = { result.data() };
    const float* src[] = { clip.data() };
    const auto r = mender::pasteBand (dest, 1, (std::int64_t) n, sr, src, 1, (std::int64_t) clip.size(), context,
                                      destStart, length, 2000.0, 4000.0, fft);
    const auto expected = mender::pasteRange ((std::int64_t) n, destStart, length, fft);
    check (r.valid && r.start == expected.start && r.end == expected.end, "reports the range it wrote", (double) (r.end - r.start));

    bool identical = true;
    for (std::size_t i = 0; i < n; ++i)
        if (((std::int64_t) i < r.start || (std::int64_t) i >= r.end) && result[i] != damaged[i])
            identical = false;
    check (identical, "audio outside that range is bit-identical", 0.0);

    const double before = powerDb (bandOf (damaged, sr, 2000, 4000, fft), {}, d0, d1);
    const double after = powerDb (bandOf (result, sr, 2000, 4000, fft), {}, d0, d1);
    const double roomTone = powerDb (bandOf (bed, sr, 2000, 4000, fft), {}, d0, d1);
    check (before - roomTone > 12.0 && before - after > 12.0, "burst (15 dB over room tone) is removed (dB)", before - after);
    check (std::abs (after - roomTone) < 3.0, "band ends up at room-tone level (dB difference)", after - roomTone);

    // Interior of the band should now be the copied audio's band.
    const auto inner0 = (std::size_t) (destStart + (std::int64_t) fft), inner1 = (std::size_t) (destStart + length - (std::int64_t) fft);
    std::vector<float> clipAligned (n, 0.0f);
    for (std::int64_t i = 0; i < (std::int64_t) clip.size(); ++i)
        if (const auto at = destStart - context + i; at >= 0 && at < (std::int64_t) n)
            clipAligned[(std::size_t) at] = clip[(std::size_t) i];
    const double match = powerDb (bandOf (result, sr, 2200, 3800, fft), bandOf (clipAligned, sr, 2200, 3800, fft), inner0, inner1)
                         - powerDb (bandOf (clipAligned, sr, 2200, 3800, fft), {}, inner0, inner1);
    check (match < -20.0, "pasted band matches the copied band (relative error dB)", match);

    // Outside the band, the 300 Hz tone and low noise are untouched.
    const double lowChange = powerDb (bandOf (result, sr, 30, 1500, fft), bandOf (damaged, sr, 30, 1500, fft), d0, d1)
                             - powerDb (bandOf (damaged, sr, 30, 1500, fft), {}, d0, d1);
    check (lowChange < -50.0, "content below the band is unchanged (relative dB)", lowChange);
    (void) burst;
}

void bandPassTest()
{
    std::printf ("\n-- band-pass listening --\n");
    const double sr = 192000.0;
    const auto n = (std::size_t) (sr * 0.5);
    std::vector<float> x (n), want (n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        want[i] = (float) (0.2 * std::sin (2 * pi * 5000.0 * t));
        x[i] = (float) (0.5 * std::sin (2 * pi * 1000.0 * t) + want[i] + 0.3 * std::sin (2 * pi * 40000.0 * t));
    }
    const auto y = bandOf (x, sr, 4000.0, 6000.0, 8192);
    const auto from = n / 4, to = 3 * n / 4;
    const double err = powerDb (y, want, from, to) - powerDb (want, {}, from, to);
    check (err < -40.0, "keeps the 5 kHz tone and drops 1 kHz and 40 kHz (error dB)", err);
}
} // namespace

int main()
{
    pasteTest (48000.0, 2048);
    pasteTest (96000.0, 4096);
    pasteTest (192000.0, 8192);
    bandPassTest();
    std::printf ("\n%s\n", failures == 0 ? "All tests passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
