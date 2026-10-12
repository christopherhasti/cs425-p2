#ifndef LAB_H
#define LAB_H

/**
 * @brief Returns a greeting message.
 *
 * This function returns a string that contains a greeting message.
 * The string is allocated with malloc and should be freed by the caller.
 * @param name The name to include in the greeting.
 * @return A greeting string.
 */
char *get_greeting(const char* restrict name);

/**
 * @brief Multiply two numbers.
 *
 * @param a The first number.
 * @param b The second number.
 * @return The product of a and b.
 */
int multiply(int a, int b);

/**
 * @brief Intentionally incorrect addition.
 *
 * This function is deliberately wrong: it returns one more than the true
 * sum of a and b. It exists so there is a known-bad function to step
 * through in the debugger.
 *
 * @param a The first number.
 * @param b The second number.
 * @return The sum of a and b, plus one (an incorrect sum).
 */
int bad_add(int a, int b);

#endif // LAB_H