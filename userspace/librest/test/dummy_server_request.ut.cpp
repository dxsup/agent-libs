/**
 * @file
 *
 * Unit tests for dummy_server_request
 *
 * @copyright Copyright (c) 2019 Sysdig Inc., All Rights Reserved
 */
#include "dummy_server_request.h"
#include <gtest.h>

namespace
{

const std::string DEFAULT_URI = "/some/uri";

} // end namespace

using test_helpers::dummy_server_request;

/**
 * Ensure that the constructor properly sets the URI.
 */
TEST(dummy_server_request_test, initial_state)
{
	dummy_server_request request(DEFAULT_URI);

	ASSERT_EQ(DEFAULT_URI, request.getURI());
}

/**
 * Ensure that stream() does not throw any exceptions.  Ensure that it returns
 * a reference to its stream.
 */
TEST(dummy_server_request_test, stream_does_not_throw_an_exception)
{
	dummy_server_request request(DEFAULT_URI);
	std::istream* stream;

	ASSERT_NO_THROW(stream = &request.stream());
	ASSERT_EQ(&request.m_stream, stream);
}

/**
 * Ensure that clientAddress() throws a std::runtime_error.
 */
TEST(dummy_server_request_test, clientAddress_throws_runtime_error)
{
	dummy_server_request request(DEFAULT_URI);

	ASSERT_THROW(request.clientAddress(), std::runtime_error);
}

/**
 * Ensure that serverAddress() throws a std::runtime_error.
 */
TEST(dummy_server_request_test, serverAddress_throws_runtime_error)
{
	dummy_server_request request(DEFAULT_URI);

	ASSERT_THROW(request.serverAddress(), std::runtime_error);
}

/**
 * Ensure that serverParams() throws a std::runtime_error.
 */
TEST(dummy_server_request_test, serverParams_throws_runtime_error)
{
	dummy_server_request request(DEFAULT_URI);

	ASSERT_THROW(request.serverParams(), std::runtime_error);
}

/**
 * Ensure that response() throws a std::runtime_error.
 */
TEST(dummy_server_request_test, response_throws_runtime_error)
{
	dummy_server_request request(DEFAULT_URI);

	ASSERT_THROW(request.serverParams(), std::runtime_error);
}

/**
 * Ensure that secure() throws a std::runtime_error.
 */
TEST(dummy_server_request_test, secure_throws_runtime_error)
{
	dummy_server_request request(DEFAULT_URI);

	ASSERT_THROW(request.secure(), std::runtime_error);
}
