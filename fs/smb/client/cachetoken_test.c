#include <kunit/test.h>

#include "cifsglob.h"
#include "smb2proto.h"

struct cachetoken_test_response {
	struct smb2_create_rsp response;
	struct create_cache_token_rsp contexts[2];
};

static struct cachetoken_test_response *cachetoken_response(struct kunit *test)
{
	struct cachetoken_test_response *packet;
	struct create_cache_token_rsp *context;

	packet = kunit_kzalloc(test, sizeof(*packet), GFP_KERNEL);
	if (!packet)
		return NULL;
	packet->response.CreateContextsOffset = cpu_to_le32(
		offsetof(struct cachetoken_test_response, contexts));
	packet->response.CreateContextsLength = cpu_to_le32(sizeof(*context));
	context = &packet->contexts[0];
	context->ccontext.NameOffset = cpu_to_le16(16);
	context->ccontext.NameLength = cpu_to_le16(16);
	context->ccontext.DataOffset = cpu_to_le16(32);
	context->ccontext.DataLength = cpu_to_le32(sizeof(context->Token));
	memcpy(context->Name, SMB2_CREATE_TAG_CACHE_TOKEN, 16);
	memset(context->Token.Token, 42, sizeof(context->Token.Token));
	packet->contexts[1] = *context;
	return packet;
}

static int cachetoken_parse(struct kunit *test,
			    struct cachetoken_test_response *packet,
			    struct smb2_cache_token *token, bool expected_received)
{
	struct kvec response = { packet, sizeof(*packet) };
	bool received = true;
	int ret;
	u16 epoch;
	u8 oplock;

	memset(token, 0xff, sizeof(*token));
	ret = smb2_parse_contexts(NULL, &response, &epoch, NULL, &oplock,
				 NULL, NULL, token, &received);
	KUNIT_EXPECT_EQ(test, received, expected_received);
	return ret;
}

static void cachetoken_valid(struct kunit *test)
{
	struct cachetoken_test_response *packet = cachetoken_response(test);
	struct smb2_cache_token token;

	KUNIT_ASSERT_NOT_NULL(test, packet);
	KUNIT_ASSERT_EQ(test, cachetoken_parse(test, packet, &token, true), 0);
	KUNIT_EXPECT_MEMEQ(test, &token, &packet->contexts[0].Token, sizeof(token));
	memset(&packet->contexts[0].Token, 0, sizeof(token));
	KUNIT_ASSERT_EQ(test, cachetoken_parse(test, packet, &token, true), 0);
	KUNIT_EXPECT_MEMEQ(test, &token, &packet->contexts[0].Token, sizeof(token));
}

static void cachetoken_unsupported(struct kunit *test)
{
	struct cachetoken_test_response *packet = cachetoken_response(test);
	struct smb2_cache_token token, empty = {};

	KUNIT_ASSERT_NOT_NULL(test, packet);
	memcpy(packet->contexts[0].Name,
	       "\x67\x8f\x91\xa4\x32\x56\x49\x85\xb6\x10\xd9\x3a\x47\x22\x6e\xc1", 16);
	packet->response.CreateContextsLength = cpu_to_le32(112);
	packet->contexts[0].ccontext.DataLength = cpu_to_le32(80);
	KUNIT_ASSERT_EQ(test, cachetoken_parse(test, packet, &token, false), 0);
	KUNIT_EXPECT_MEMEQ(test, &token, &empty, sizeof(token));
	packet->response.CreateContextsLength = 0;
	packet->response.CreateContextsOffset = 0;
	KUNIT_ASSERT_EQ(test, cachetoken_parse(test, packet, &token, false), 0);
	KUNIT_EXPECT_MEMEQ(test, &token, &empty, sizeof(token));
}

static void cachetoken_unsolicited(struct kunit *test)
{
	struct cachetoken_test_response *packet = cachetoken_response(test);
	struct kvec response = { packet, sizeof(*packet) };
	bool received = true;
	u16 epoch;
	u8 oplock;

	KUNIT_ASSERT_NOT_NULL(test, packet);
	KUNIT_ASSERT_EQ(test, smb2_parse_contexts(NULL, &response, &epoch, NULL,
						 &oplock, NULL, NULL, NULL, &received), 0);
	KUNIT_EXPECT_FALSE(test, received);
	packet->contexts[0].ccontext.DataLength = cpu_to_le32(31);
	received = true;
	KUNIT_EXPECT_EQ(test, smb2_parse_contexts(NULL, &response, &epoch, NULL,
						 &oplock, NULL, NULL, NULL, &received), -EINVAL);
	KUNIT_EXPECT_FALSE(test, received);
}

static void cachetoken_duplicate(struct kunit *test)
{
	struct cachetoken_test_response *packet = cachetoken_response(test);
	struct smb2_cache_token token, empty = {};

	KUNIT_ASSERT_NOT_NULL(test, packet);
	packet->response.CreateContextsLength = cpu_to_le32(sizeof(packet->contexts));
	packet->contexts[0].ccontext.Next = cpu_to_le32(sizeof(packet->contexts[0]));
	KUNIT_EXPECT_EQ(test, cachetoken_parse(test, packet, &token, false), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &token, &empty, sizeof(token));
}

static void cachetoken_malformed(struct kunit *test)
{
	struct cachetoken_test_response *packet = cachetoken_response(test);
	struct smb2_cache_token token, empty = {};
	static const unsigned int lengths[] = { 0, 31, 33, 80 };
	unsigned int index;

	KUNIT_ASSERT_NOT_NULL(test, packet);
	packet->response.CreateContextsLength = cpu_to_le32(sizeof(packet->contexts));
	for (index = 0; index < ARRAY_SIZE(lengths); index++) {
		packet->contexts[0].ccontext.DataLength = cpu_to_le32(lengths[index]);
		KUNIT_EXPECT_EQ(test, cachetoken_parse(test, packet, &token, false), -EINVAL);
		KUNIT_EXPECT_MEMEQ(test, &token, &empty, sizeof(token));
	}
	packet->response.CreateContextsLength = cpu_to_le32(sizeof(packet->contexts[0]));
	packet->contexts[0].ccontext.DataLength = cpu_to_le32(sizeof(token));
	packet->contexts[0].ccontext.Next = cpu_to_le32(33);
	KUNIT_EXPECT_EQ(test, cachetoken_parse(test, packet, &token, false), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &token, &empty, sizeof(token));
	packet->contexts[0].ccontext.Next = cpu_to_le32(sizeof(packet->contexts[0]));
	KUNIT_EXPECT_EQ(test, cachetoken_parse(test, packet, &token, false), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &token, &empty, sizeof(token));
	packet->contexts[0].ccontext.Next = 0;
	packet->response.CreateContextsOffset = cpu_to_le32(sizeof(*packet));
	KUNIT_EXPECT_EQ(test, cachetoken_parse(test, packet, &token, false), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &token, &empty, sizeof(token));
}

static struct kunit_case cachetoken_cases[] = {
	KUNIT_CASE(cachetoken_valid),
	KUNIT_CASE(cachetoken_unsupported),
	KUNIT_CASE(cachetoken_unsolicited),
	KUNIT_CASE(cachetoken_duplicate),
	KUNIT_CASE(cachetoken_malformed),
	{}
};

static struct kunit_suite cachetoken_suite = {
	.name = "smb2_cachetoken",
	.test_cases = cachetoken_cases,
};

kunit_test_suite(cachetoken_suite);
