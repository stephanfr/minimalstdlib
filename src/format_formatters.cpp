// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <format>

#include <ctype.h>
#include <string.h>

#include <fixed_string>

namespace FMT_FORMATTERS_NAMESPACE
{
    using arg_format_options = ::MINIMAL_STD_NAMESPACE::arg_format_options;

    constexpr char LOWER_CASE_NUMERIC_CONVERSION_DIGITS[37] = "0123456789abcdefghijklmnopqrstuvwxyz";
    constexpr char UPPER_CASE_NUMERIC_CONVERSION_DIGITS[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    constexpr char PLUS_SIGN[] = {"+"};
    constexpr char MINUS_SIGN[] = {"-"};
    constexpr char SPACE_CHAR[] = {" "};
    constexpr char NO_SIGN_OR_SPACE[] = {""};

    //
    //  Numbers are rendered most significant character first into a numeric_text, then emitted with
    //      their fill.  Writing forward means a buffer that fills up truncates the end of the number,
    //      never its sign or leading digits.
    //

    template <size_t CAPACITY>
    struct numeric_text_of
    {
        const char *sign = "";
        const char *prefix = "";
        char digits[CAPACITY];
        size_t length = 0;
        size_t trailing_zeros = 0; //  counted rather than stored, so a large precision need not fit
        bool finite = true;        //  inf and nan are padded with spaces, never zeros

        void push_back(char c)
        {
            if (length < CAPACITY)
            {
                digits[length++] = c;
            }
        }
    };

    //  Enough for 64 binary digits, a '.' and the decimals we compute.

    using numeric_text = numeric_text_of<80>;

    //  The 309 integer digits of the largest double and a '.'.  Kept apart so that formatting an integer
    //      or an ordinary float does not pay for this much stack.

    using large_float_text = numeric_text_of<310>;

    //
    //  Function to determine integer prefixes for binary, octal or hex
    //

    const char *IntegerPrefix(const arg_format_options &format, bool value_is_zero)
    {
        //  If this is hex, octal or binary and alt is specified, then we need to add the prefix

        if (format.is_alt() && format.type_specifier().has_value())
        {
            switch (format.type_specifier().value())
            {
            case 'b':
                return "0b";

            case 'B':
                return "0B";

            case 'o':
                //  The octal prefix is a leading zero, which zero already has

                return value_is_zero ? "" : "0";

            case 'x':
                return "0x";

            case 'X':
                return "0X";
            }
        }

        return "";
    }

    const char *DetermineSign(bool is_negative, const arg_format_options &format)
    {
        //  If the value is negative, then we will always include the unary minus sign

        if (is_negative)
        {
            return MINUS_SIGN;
        }

        //  If the sign is specified, then we may need to add either a plus or space

        if (format.sign().has_value())
        {
            if (format.sign().value() == arg_format_options::sign_treatment::always_plus)
            {
                return PLUS_SIGN;
            }
            else if (format.sign().value() == arg_format_options::sign_treatment::space)
            {
                return SPACE_CHAR;
            }
        }

        //  If we are down here, then no sign or space before the number

        return NO_SIGN_OR_SPACE;
    }

    //
    //  Function to align, pad and emit a rendered number
    //

    //
    //  Appends a character count times, but no more than the buffer can still hold: a width or precision near
    //      UINT32_MAX would otherwise loop billions of times on a full buffer.
    //

    void AppendRepeated(minstd::string &buffer, char c, size_t count)
    {
        const size_t room = (buffer.max_size() > buffer.size()) ? buffer.max_size() - buffer.size() : 0;

        for (size_t i = minstd::min(count, room); i > 0; i--)
        {
            const size_t before = buffer.size();

            buffer.push_back(c);

            if (buffer.size() == before)
            {
                return; //  the buffer could not grow (a dynamic string out of memory): retrying cannot help
            }
        }
    }

    template <size_t CAPACITY>
    void EmitNumber(minstd::string &buffer, const numeric_text_of<CAPACITY> &number, const arg_format_options &format)
    {
        const size_t sign_length = strnlen(number.sign, 2);
        const size_t prefix_length = strnlen(number.prefix, 3);

        const size_t number_length = sign_length + prefix_length + number.length + number.trailing_zeros;

        size_t fill_before = 0;
        size_t zero_fill = 0;
        size_t fill_after = 0;

        if (format.width().has_value() && (format.width().value() > number_length))
        {
            const size_t fill_count = format.width().value() - number_length;

            if (format.is_zero_fill() && number.finite)
            {
                zero_fill = fill_count;
            }
            else if (format.is_zero_fill() || format.is_right_aligned())
            {
                fill_before = fill_count; //  zero fill does not apply to inf and nan, which pad with spaces
            }
            else if (format.is_center_aligned())
            {
                fill_before = fill_count / 2;
                fill_after = fill_count - fill_before;
            }
            else
            {
                fill_after = fill_count;
            }
        }

        const char fill = (format.fill().has_value() && !format.is_zero_fill()) ? format.fill().value() : ' ';

        AppendRepeated(buffer, fill, fill_before);

        buffer += number.sign;
        buffer += number.prefix;

        AppendRepeated(buffer, '0', zero_fill);

        for (size_t i = 0; i < number.length; i++)
        {
            buffer.push_back(number.digits[i]);
        }

        AppendRepeated(buffer, '0', number.trailing_zeros);

        AppendRepeated(buffer, fill, fill_after);
    }

    //
    //  Function to render an unsigned value most significant digit first
    //

    template <size_t CAPACITY, typename T>
    void AppendDigits(numeric_text_of<CAPACITY> &number, T value, unsigned base, const char *numeric_conversion_digits)
    {
        char reversed[64];
        size_t count = 0;

        do
        {
            reversed[count++] = numeric_conversion_digits[value % base];
            value = value / base;
        } while (value != 0);

        while (count > 0)
        {
            number.push_back(reversed[--count]);
        }
    }

    //
    //  Floats are rendered from their IEEE 754 bits in integer arithmetic, so the digits are exact.
    //

    //  At or above 2^64 a double is an integer, m * 2^shift.  It is built in base 10^9 so its digits are exact.

    void LargeFloatToString(minstd::string &buffer, uint64_t mantissa, uint32_t shift, const char *sign, uint32_t precision, const arg_format_options &format)
    {
        static constexpr uint32_t LIMB_BASE = 1000000000;
        static constexpr size_t MAX_LIMBS = 35;       //  10^315 > 2^1024
        static constexpr uint32_t MAX_LIMB_SHIFT = 29; //  a limb below 2^30, shifted, plus a carry fits 64 bits

        uint32_t limbs[MAX_LIMBS]; //  least significant first
        size_t limb_count = 0;

        for (uint64_t rest = mantissa; rest != 0; rest /= LIMB_BASE)
        {
            limbs[limb_count++] = static_cast<uint32_t>(rest % LIMB_BASE);
        }

        for (uint32_t remaining = shift; remaining > 0;)
        {
            const uint32_t step = minstd::min(remaining, MAX_LIMB_SHIFT);
            uint64_t carry = 0;

            for (size_t i = 0; i < limb_count; i++)
            {
                const uint64_t shifted = (static_cast<uint64_t>(limbs[i]) << step) + carry;

                limbs[i] = static_cast<uint32_t>(shifted % LIMB_BASE);
                carry = shifted / LIMB_BASE;
            }

            if (carry != 0)
            {
                limbs[limb_count++] = static_cast<uint32_t>(carry);
            }

            remaining -= step;
        }

        large_float_text number;

        number.sign = sign;

        AppendDigits(number, limbs[limb_count - 1], 10, LOWER_CASE_NUMERIC_CONVERSION_DIGITS);

        for (size_t limb = limb_count - 1; limb > 0; limb--)
        {
            for (uint32_t divisor = LIMB_BASE / 10; divisor > 0; divisor /= 10)
            {
                number.push_back('0' + (limbs[limb - 1] / divisor) % 10);
            }
        }

        if (precision > 0)
        {
            number.push_back('.');
            number.trailing_zeros = precision;
        }

        EmitNumber(buffer, number, format);
    }

    void FloatToString(minstd::string &buffer, double value, const arg_format_options &format)
    {
        static constexpr uint64_t MANTISSA_MASK = (uint64_t{1} << 52) - 1;
        static constexpr uint32_t INFINITY_OR_NAN = 0x7FF;
        static constexpr uint32_t UNIT_EXPONENT = 1075; //  a biased exponent at which the mantissa counts units

        const uint64_t bits = __builtin_bit_cast(uint64_t, value);
        const uint32_t biased_exponent = static_cast<uint32_t>((bits >> 52) & 0x7FF);
        const uint64_t fraction_bits = bits & MANTISSA_MASK;

        //  The sign bit, so -0.0 and -nan keep their sign as they do in std::format

        const char *sign = DetermineSign((bits >> 63) != 0, format);

        numeric_text number;

        number.sign = sign;

        if (biased_exponent == INFINITY_OR_NAN)
        {
            const char *name = (fraction_bits != 0) ? "nan" : "inf";

            number.finite = false;
            number.push_back(name[0]);
            number.push_back(name[1]);
            number.push_back(name[2]);

            EmitNumber(buffer, number, format);
            return;
        }

        const uint32_t precision = format.precision().has_value() ? format.precision().value() : 0;

        //  The magnitude is exactly mantissa * 2^(exponent - UNIT_EXPONENT); a subnormal has exponent 1 and no implicit bit

        const uint64_t mantissa = (biased_exponent == 0) ? fraction_bits : (fraction_bits | (uint64_t{1} << 52));
        const uint32_t exponent = (biased_exponent == 0) ? 1 : biased_exponent;

        if (exponent >= UNIT_EXPONENT + 12)
        {
            LargeFloatToString(buffer, mantissa, exponent - UNIT_EXPONENT, sign, precision, format);
            return;
        }

        static uint64_t const pow10[] = {1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
                                         100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL,
                                         1000000000000ULL, 10000000000000ULL, 100000000000000ULL,
                                         1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL,
                                         1000000000000000000ULL, 10000000000000000000ULL};

        //  Up to 19 decimals are computed exactly from the binary value, as std::format prints them; any
        //      further ones are zeros.

        const uint32_t computed_digits = minstd::min(precision, (uint32_t)19);
        const uint64_t decimal_multiplier = pow10[computed_digits];

        uint64_t units = 0;
        uint64_t decimals = 0;
        bool round_up = false;

        if (exponent >= UNIT_EXPONENT)
        {
            units = mantissa << (exponent - UNIT_EXPONENT);
        }
        else
        {
            const uint32_t fraction_shift = UNIT_EXPONENT - exponent;

            //  Otherwise the value is below 2^-75, and even 10^19 times it rounds to zero

            if (fraction_shift < 128)
            {
                using uint128 = unsigned __int128;

                const uint128 one = static_cast<uint128>(1) << fraction_shift;
                const uint128 whole = static_cast<uint128>(mantissa) >> fraction_shift;
                const uint128 fraction = static_cast<uint128>(mantissa) & (one - 1);

                //  fraction < 2^53, so the product is below 2^117

                const uint128 product = fraction * decimal_multiplier;
                const uint128 remainder = product & (one - 1);
                const uint128 half = one >> 1;

                units = static_cast<uint64_t>(whole);
                decimals = static_cast<uint64_t>(product >> fraction_shift);

                //  Round half to even, as std::format does for an exact tie (2.5 -> "2", 0.125 -> "0.12")

                const uint64_t last_digit = (computed_digits > 0) ? decimals : units;

                round_up = (remainder > half) || ((remainder == half) && ((last_digit & 1) != 0));
            }
        }

        if (round_up)
        {
            if (++decimals == decimal_multiplier)
            {
                decimals = 0;
                units++; //  cannot wrap: a double with a fraction is below 2^52
            }
        }

        AppendDigits(number, units, 10, LOWER_CASE_NUMERIC_CONVERSION_DIGITS);

        if (precision > 0)
        {
            number.push_back('.');

            for (uint32_t i = computed_digits; i > 0; i--)
            {
                number.push_back('0' + (decimals / pow10[i - 1]) % 10);
            }

            number.trailing_zeros = precision - computed_digits;
        }

        EmitNumber(buffer, number, format);
    }

    //
    //  Template function to convert unsigned integers to string.  Works for all unsigned integer types.
    //

    template <typename T>
    void UnsignedMagnitudeToString(minstd::string &buffer, T magnitude, bool is_negative, const arg_format_options &format)
    {
        //  Insure the desired base is legal, only 2 to 36 is supported

        if (!format.integer_base().has_value() ||
            ((format.integer_base().value() < 2) || (format.integer_base().value() > 36)))
        {
            buffer += "{Invalid base}";
            return;
        }

        const char *numeric_conversion_digits = (format.type_specifier().has_value() && ((format.type_specifier().value() == 'X') || (format.type_specifier().value() == 'B'))) ? UPPER_CASE_NUMERIC_CONVERSION_DIGITS : LOWER_CASE_NUMERIC_CONVERSION_DIGITS;

        numeric_text number;

        number.sign = DetermineSign(is_negative, format);
        number.prefix = IntegerPrefix(format, magnitude == 0);

        AppendDigits(number, magnitude, format.integer_base().value(), numeric_conversion_digits);

        EmitNumber(buffer, number, format);
    }

    template <typename T>
    void UnsignedIntToString(minstd::string &buffer, T value, const arg_format_options &format)
    {
        UnsignedMagnitudeToString(buffer, value, false, format);
    }

    //
    //  Template function to convert signed integers to string.  Works for all signed integer types.
    //

    template <typename T>
    void SignedIntToString(minstd::string &buffer, T value, const arg_format_options &format)
    {
        //  We need to convert the value to an unsigned type to handle the negative case

        typedef typename minstd::make_unsigned<T>::type unsigned_T;

        if (value < 0)
        {
            UnsignedMagnitudeToString(buffer, (unsigned_T)(unsigned_T(0) - (unsigned_T)value), true, format);
        }
        else
        {
            UnsignedMagnitudeToString(buffer, (unsigned_T)value, false, format);
        }
    }

    //
    //  Template function to append strings to the buffer
    //

    void FormattedStringAppend(minstd::string &buffer, const char *value, size_t length, const arg_format_options &format_options)
    {
        //  A precision on a string is the most characters to print

        if (format_options.precision().has_value() && (format_options.precision().value() < length))
        {
            length = format_options.precision().value();
        }

        //  Determine if fill is needed for alignment and if yes, the amount of fill needed before and/or after the string

        size_t fill_after_count = 0;
        size_t fill_before_count = 0;

        if (format_options.width().has_value() && (format_options.width().value() > length))
        {
            size_t fill_count = format_options.width().value() - length;

            if (format_options.alignment().has_value())
            {
                switch (format_options.alignment().value())
                {
                case arg_format_options::align::left:
                    fill_after_count = fill_count;
                    break;

                case arg_format_options::align::right:
                    fill_before_count = fill_count;
                    break;

                case arg_format_options::align::center:
                    fill_before_count = fill_count / 2;
                    fill_after_count = fill_count - fill_before_count;
                    break;
                }
            }
        }

        AppendRepeated(buffer, format_options.fill().value(), fill_before_count);

        buffer.append(value, length);

        AppendRepeated(buffer, format_options.fill().value(), fill_after_count);
    }

    //
    //  Specializations for specific argument formatters
    //

    template <>
    void fmt_arg_base<const char>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        const char local_buffer[] = { value_, 0 };

        FormattedStringAppend(buffer, local_buffer, 1, format_options);
    }

    template <>
    void fmt_arg_base<const char *>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        FormattedStringAppend(buffer, value_, strnlen(value_, SIZE_MAX), format_options);
    }

    template <>
    void fmt_arg_base<const minstd::string &>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        FormattedStringAppend(buffer, value_, value_.length(), format_options);
    }

    template <>
    void fmt_arg_base<const minstd::string_view &>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        FormattedStringAppend(buffer, value_.data(), value_.length(), format_options);
    }

    template <>
    void fmt_arg_base<const uint64_t>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        UnsignedIntToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const int64_t>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        SignedIntToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const uint32_t>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        UnsignedIntToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const int32_t>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        SignedIntToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const uint16_t>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        UnsignedIntToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const int16_t>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        SignedIntToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const uint8_t>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        UnsignedIntToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const int8_t>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        SignedIntToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const float>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        FloatToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const double>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        FloatToString(buffer, value_, format_options);
    }

    template <>
    void fmt_arg_base<const bool>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        if (value_)
        {
            FormattedStringAppend(buffer, "true", 4, format_options);
        }
        else
        {
            FormattedStringAppend(buffer, "false", 5, format_options);
        }
    }

    template <>
    void fmt_arg_base<const void *>::AppendInternal(minstd::string &buffer, const ::MINIMAL_STD_NAMESPACE::arg_format_options &format_options) const
    {
        UnsignedIntToString(buffer, reinterpret_cast<uint64_t>(value_), format_options);
    }

} // namespace FMT_FORMATTERS_NAMESPACE
