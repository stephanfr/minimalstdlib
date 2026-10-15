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

    struct numeric_text
    {
        //  Enough for 64 binary digits, a '.' and the decimals we compute.  Runs of zeros are counted
        //      rather than stored, so a large double or a large precision need not fit.

        static constexpr size_t CAPACITY = 80;

        const char *sign = "";
        const char *prefix = "";
        char digits[CAPACITY];
        size_t length = 0;
        size_t fraction_start = 0; //  digits[fraction_start..length) follow the integer zeros
        size_t integer_zeros = 0;  //  zeros at the end of the integer part
        size_t trailing_zeros = 0; //  zeros at the end of the fraction
        bool finite = true;        //  inf and nan are padded with spaces, never zeros

        void push_back(char c)
        {
            if (length < CAPACITY)
            {
                digits[length++] = c;
            }
        }
    };

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
            buffer.push_back(c);
        }
    }

    void EmitNumber(minstd::string &buffer, const numeric_text &number, const arg_format_options &format)
    {
        const size_t sign_length = strnlen(number.sign, 2);
        const size_t prefix_length = strnlen(number.prefix, 3);

        const size_t number_length = sign_length + prefix_length + number.length + number.integer_zeros + number.trailing_zeros;

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
            else if (format.is_right_aligned())
            {
                fill_before = fill_count;
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

        const size_t fraction_start = number.fraction_start > 0 ? number.fraction_start : number.length;

        for (size_t i = 0; i < fraction_start; i++)
        {
            buffer.push_back(number.digits[i]);
        }

        AppendRepeated(buffer, '0', number.integer_zeros);

        for (size_t i = fraction_start; i < number.length; i++)
        {
            buffer.push_back(number.digits[i]);
        }

        AppendRepeated(buffer, '0', number.trailing_zeros);

        AppendRepeated(buffer, fill, fill_after);
    }

    //
    //  Function to render an unsigned value most significant digit first
    //

    template <typename T>
    void AppendDigits(numeric_text &number, T value, unsigned base, const char *numeric_conversion_digits)
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
    //  Template function to convert floats to string.  Works for all floating point types.
    //

    template <typename T>
    void FloatToString(minstd::string &buffer, const T &value, const arg_format_options &format)
    {
        static uint64_t const pow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

        numeric_text number;

        //  The sign bit, not value < 0, so -0.0 and -nan keep their sign as they do in std::format

        const bool is_negative = __builtin_signbit(value);

        //  NaN compares unequal to itself

        if (value != value)
        {
            number.sign = DetermineSign(is_negative, format);
            number.finite = false;
            number.push_back('n');
            number.push_back('a');
            number.push_back('n');

            EmitNumber(buffer, number, format);
            return;
        }

        const double magnitude = is_negative ? -(double)value : (double)value;

        number.sign = DetermineSign(is_negative, format);

        if (magnitude > 1.7976931348623157e308)
        {
            number.finite = false;
            number.push_back('i');
            number.push_back('n');
            number.push_back('f');

            EmitNumber(buffer, number, format);
            return;
        }

        const uint32_t precision = format.precision().has_value() ? format.precision().value() : 0;

        //  Below 2^64 the integer part fits a uint64_t and the fraction (magnitude - units) is exact, so the
        //      decimals come from the fraction alone: the integer part takes no rounding error however large it
        //      is.  A double carries about 17 significant digits, so up to 17 decimals are computed and any
        //      further ones are zeros.

        if (magnitude < 18446744073709551616.0)
        {
            static uint64_t const pow10[] = {1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
                                             100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL,
                                             1000000000000ULL, 10000000000000ULL, 100000000000000ULL,
                                             1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL};

            const uint32_t computed_digits = minstd::min(precision, (uint32_t)17);
            const uint64_t decimal_multiplier = pow10[computed_digits];

            uint64_t units = (uint64_t)magnitude;
            const double fraction = magnitude - (double)units;

            const double scaled = fraction * (double)decimal_multiplier;
            uint64_t decimals = (uint64_t)scaled;
            const double remainder = scaled - (double)decimals;

            //  Round half to even, as std::format does for an exact tie (2.5 -> "2", 0.125 -> "0.12")

            const uint64_t last_digit = (computed_digits > 0) ? decimals : units;

            if ((remainder > 0.5) || ((remainder == 0.5) && ((last_digit & 1) != 0)))
            {
                if (++decimals == decimal_multiplier)
                {
                    decimals = 0;
                    units++; //  cannot wrap: doubles this close to 2^64 have no fraction
                }
            }

            AppendDigits(number, units, 10, LOWER_CASE_NUMERIC_CONVERSION_DIGITS);

            if (precision > 0)
            {
                number.fraction_start = number.length;
                number.push_back('.');

                for (uint32_t i = computed_digits; i > 0; i--)
                {
                    number.push_back('0' + (decimals / pow10[i - 1]) % 10);
                }

                number.trailing_zeros = precision - computed_digits;
            }
        }
        else
        {
            //  Too large for the integer: print its leading 17 significant digits, which is all a double
            //      carries, then zeros for the rest of the magnitude.  The scale is divided out in exact
            //      powers of ten (up to 1e22 is exact) so the leading digits pick up as little error as we can.

            uint32_t exponent = 0;
            double power = 1;

            while (magnitude / power >= 1e17)
            {
                power *= 10;
                exponent++;
            }

            double scaled = magnitude;

            for (uint32_t remaining = exponent; remaining > 0;)
            {
                const uint32_t step = minstd::min(remaining, (uint32_t)22);
                double divisor = 1;

                for (uint32_t i = 0; i < step; i++)
                {
                    divisor *= 10;
                }

                scaled /= divisor;
                remaining -= step;
            }

            AppendDigits(number, (uint64_t)(scaled + 0.5), 10, LOWER_CASE_NUMERIC_CONVERSION_DIGITS);

            number.integer_zeros = exponent;

            if (precision > 0)
            {
                number.fraction_start = number.length;
                number.push_back('.');
                number.trailing_zeros = precision;
            }
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
