#include "lib.h"

#include "sieve-code.h"
#include "sieve-commands.h"
#include "sieve-commands-private.h"
#include "sieve-validator.h"
#include "sieve-generator.h"
#include "sieve-interpreter.h"
#include "sieve-code-dumper.h"

/* Size test 
 *
 * Syntax:
 *    size <":over" / ":under"> <limit: number>
 */

static bool tst_size_registered
	(struct sieve_validator *validator, struct sieve_command_registration *cmd_reg);
static bool tst_size_pre_validate
	(struct sieve_validator *validator, struct sieve_command_context *tst);
static bool tst_size_validate
	(struct sieve_validator *validator, struct sieve_command_context *tst);
static bool tst_size_generate
	(struct sieve_generator *generator, struct sieve_command_context *ctx); 

const struct sieve_command tst_size = { 
	"size", 
	SCT_TEST, 
	1, 0, FALSE, FALSE,
	tst_size_registered, 
	tst_size_pre_validate,
	tst_size_validate, 
	tst_size_generate, 
	NULL 
};

/* Opcodes */

static bool tst_size_opcode_dump
	(const struct sieve_opcode *opcode, 
		const struct sieve_dumptime_env *denv, sieve_size_t *address);
static bool tst_size_opcode_execute
	(const struct sieve_opcode *opcode, 
		const struct sieve_runtime_env *renv, sieve_size_t *address);

const struct sieve_opcode tst_size_over_opcode = { 
	"SIZE-OVER",
	SIEVE_OPCODE_SIZE_OVER,
	NULL, 0,
	tst_size_opcode_dump, 
	tst_size_opcode_execute 
};

const struct sieve_opcode tst_size_under_opcode = {
	"SIZE-UNDER",
	SIEVE_OPCODE_SIZE_UNDER,
	NULL, 0, 
	tst_size_opcode_dump, 
	tst_size_opcode_execute 
};

/* Context structures */

struct tst_size_context_data {
	enum { SIZE_UNASSIGNED, SIZE_UNDER, SIZE_OVER } type;
};

#define TST_SIZE_ERROR_DUP_TAG \
	"exactly one of the ':under' or ':over' tags must be specified for the size test, but more were found"

/* Tag validation */

static bool tst_size_validate_over_tag
(struct sieve_validator *validator, struct sieve_ast_argument **arg, 
	struct sieve_command_context *tst)
{
	struct tst_size_context_data *ctx_data = (struct tst_size_context_data *) tst->data;	
	
	if ( ctx_data->type != SIZE_UNASSIGNED ) {
		sieve_command_validate_error(validator, tst, TST_SIZE_ERROR_DUP_TAG);
		return FALSE;		
	}
	
	ctx_data->type = SIZE_OVER;
	
	/* Delete this tag */
	*arg = sieve_ast_arguments_detach(*arg, 1);
	
	return TRUE;
}

static bool tst_size_validate_under_tag
(struct sieve_validator *validator, struct sieve_ast_argument **arg ATTR_UNUSED, 
	struct sieve_command_context *tst)
{
	struct tst_size_context_data *ctx_data = (struct tst_size_context_data *) tst->data;	
	
	if ( ctx_data->type != SIZE_UNASSIGNED ) {
		sieve_command_validate_error(validator, tst, TST_SIZE_ERROR_DUP_TAG);
		return FALSE;		
	}
	
	ctx_data->type = SIZE_UNDER;
	
	/* Delete this tag */
	*arg = sieve_ast_arguments_detach(*arg, 1);
		
	return TRUE;
}

/* Test registration */

static const struct sieve_argument size_over_tag = 
	{ "over", NULL, tst_size_validate_over_tag, NULL, NULL };
static const struct sieve_argument size_under_tag = 
	{ "under", NULL, tst_size_validate_under_tag, NULL, NULL };

static bool tst_size_registered
	(struct sieve_validator *validator, struct sieve_command_registration *cmd_reg) 
{
	sieve_validator_register_tag(validator, cmd_reg, &size_over_tag, 0); 	
	sieve_validator_register_tag(validator, cmd_reg, &size_under_tag, 0); 	

	return TRUE;
}

/* Test validation */

static bool tst_size_pre_validate
	(struct sieve_validator *validator ATTR_UNUSED, struct sieve_command_context *tst) 
{
	struct tst_size_context_data *ctx_data;
	
	/* Assign context */
	ctx_data = p_new(sieve_command_pool(tst), struct tst_size_context_data, 1);
	ctx_data->type = SIZE_UNASSIGNED;
	tst->data = ctx_data;

	return TRUE;
}

static bool tst_size_validate
	(struct sieve_validator *validator, struct sieve_command_context *tst) 
{
	struct tst_size_context_data *ctx_data = (struct tst_size_context_data *) tst->data;
	struct sieve_ast_argument *arg = tst->first_positional;
	
	if ( ctx_data->type == SIZE_UNASSIGNED ) {
		sieve_command_validate_error(validator, tst, 
			"the size test requires either the :under or the :over tag to be specified");
		return FALSE;		
	}
		
	if ( !sieve_validate_positional_argument
		(validator, tst, arg, "limit", 1, SAAT_NUMBER) ) {
		return FALSE;
	}
	sieve_validator_argument_activate(validator, tst, arg, FALSE);
	
	return TRUE;
}

/* Test generation */

bool tst_size_generate
	(struct sieve_generator *generator, struct sieve_command_context *ctx) 
{
	struct tst_size_context_data *ctx_data = (struct tst_size_context_data *) ctx->data;

	if ( ctx_data->type == SIZE_OVER ) 
		sieve_generator_emit_opcode(generator, &tst_size_over_opcode);
	else
		sieve_generator_emit_opcode(generator, &tst_size_under_opcode);

 	/* Generate arguments */
    if ( !sieve_generate_arguments(generator, ctx, NULL) )
        return FALSE;
	  
	return TRUE;
}

/* Code dump */

static bool tst_size_opcode_dump
(const struct sieve_opcode *opcode,
	const struct sieve_dumptime_env *denv, sieve_size_t *address)
{
    sieve_code_dumpf(denv, "%s", opcode->mnemonic);
	sieve_code_descend(denv);
	
	return 
		sieve_opr_number_dump(denv, address);
}

/* Code execution */

static bool tst_size_get
(const struct sieve_runtime_env *renv, sieve_size_t *size) 
{
	uoff_t psize;

	if ( mail_get_physical_size(renv->msgdata->mail, &psize) < 0 )
		return FALSE;

	*size = psize;
  
	return TRUE;
}

static bool tst_size_opcode_execute
(const struct sieve_opcode *opcode,
	const struct sieve_runtime_env *renv, sieve_size_t *address)
{
	sieve_size_t mail_size, limit;
	
	printf("%s\n", opcode->mnemonic);
	
	if ( !sieve_opr_number_read(renv->sbin, address, &limit) ) 
		return FALSE;	
	
	if ( !tst_size_get(renv, &mail_size) )
		return FALSE;
	
	if ( opcode == &tst_size_over_opcode )
		sieve_interpreter_set_test_result(renv->interp, (mail_size > limit));
	else
		sieve_interpreter_set_test_result(renv->interp, (mail_size < limit));

	return TRUE;
}

