/*
 * init_sec.c --- initialize security context
 * 
 * $Source$
 * $Author$
 * $Header$
 * 
 * Copyright 1991 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * For copying and distribution information, please see the file
 * <krb5/copyright.h>.
 *
 */

#include <gssapi.h>

extern krb5_flags    krb5_kdc_default_options;

/*
 * To do in the future:
 *
 * 	* Support replay cache
 *
 * 	* Support delegation of credentials
 *
 * 	* Do something with time_rec
 *
 * 	* Should handle Kerberos error packets being sent back and
 * 	forth.
 */

gss_cred_id_t	gss_default_credentials = {
	(krb5_principal) NULL, (gss_OID) NULL, 0, (krb5_ccache) NULL,
	(krb5_kvno) 0, { (krb5_keytype) 0, 0, (krb5_octet *) NULL }
};
		

OM_uint32 gss_init_sec_context(minor_status, claimant_cred_handle,
			       context_handle, target_name,
			       mech_type, req_flags, time_req,
			       channel, input_token,
			       actual_mech_type, output_token,
			       ret_flags, time_rec)
	OM_uint32	*minor_status;
	gss_cred_id_t	claimant_cred_handle;
	gss_ctx_id_t	*context_handle;
	gss_name_t	target_name;
	gss_OID		mech_type;
	int		req_flags;
	int		time_req;
	gss_channel_bindings	channel;
	gss_buffer_t	input_token;
	gss_OID		*actual_mech_type;
	gss_buffer_t	output_token;
	int    		*ret_flags;
	OM_uint32	*time_rec;
{
	krb5_flags		kdc_options = krb5_kdc_default_options;
	krb5_flags		ap_req_options = 0;
	krb5_ccache		ccache;
	krb5_creds 		creds;
	krb5_authenticator	authent;
	krb5_data		inbuf, outbuf;
	krb5_ap_rep_enc_part	*repl;
	OM_uint32		retval;
	gss_ctx_id_t	context;
	
	*minor_status = 0;

	if (!context_handle) {
		/*
		 * This is first call to init_sec_context
		 *
		 * We only handle Kerberos V5...
		 */
		if ((mech_type != GSS_C_NULL_OID) &&
		    !gss_compare_OID(mech_type, &gss_OID_krb5)) {
			return(gss_make_re(GSS_RE_BAD_MECH));
		}
		if (actual_mech_type)
			*actual_mech_type = &gss_OID_krb5;
		/*
		 * Sanitize the incoming flags
		 *
		 * We don't support delegation or replay detection --- yet.
		 */
		req_flags &= ~GSS_C_DELEG_FLAG;
		req_flags &= ~GSS_C_REPLAY_FLAG; 
		/*
		 * If no credentials were passed in, get our own
		 */
		if (claimant_cred_handle.ccache)
			ccache = claimant_cred_handle.ccache;
		else {
			/*
			 * Default (or NULL) credentials, we need to
			 * fill in with defaults.
			 */
			if (*minor_status = krb5_cc_default(&ccache)) {
				return(gss_make_re(GSS_RE_FAILURE));
			}
			claimant_cred_handle.ccache = ccache;
			if (*minor_status =
			    krb5_cc_get_principal(ccache,
						  &claimant_cred_handle.principal))
				return(gss_make_re(GSS_RE_FAILURE));
		}
		/*
		 * Allocate the context handle structure
		 */
		if (!(context = malloc(sizeof(struct gss_ctx_id_desc)))) {
			*minor_status = ENOMEM;
			return(gss_make_re(GSS_RE_FAILURE));
		}
		context->mech_type = &gss_OID_krb5;
		context->state =  GSS_KRB_STATE_DOWN;
		/*
		 * Fill in context handle structure
		 */
		if (*minor_status =
		    krb5_copy_principal(claimant_cred_handle.principal,
					&context->me))
			return(gss_make_re(GSS_RE_FAILURE));
		if (*minor_status =
		    krb5_copy_principal(target_name,
					&context->him))
			return(gss_make_re(GSS_RE_FAILURE));
		context->flags = req_flags | GSS_C_CONF_FLAG;;
		context->am_client = 1;
		context->session_key = NULL;
		context->my_address.addrtype = channel.sender_addrtype;
		context->my_address.length = channel.sender_address.length;
		if (!(context->my_address.contents =
		      malloc(context->my_address.length))) {
			xfree(context);
			return(gss_make_re(GSS_RE_FAILURE));
		}
		memcpy((char *) context->my_address.contents,
		       (char *) channel.sender_address.value,
		       context->my_address.length);
		context->his_address.addrtype = channel.receiver_addrtype;
		context->his_address.length = channel.receiver_address.length;
		if (!(context->his_address.contents =
		      malloc(context->my_address.length))) {
			xfree(context->my_address.contents);
			xfree(context);
			return(gss_make_re(GSS_RE_FAILURE));
		}
		memcpy((char *) context->his_address.contents,
		       (char *) channel.receiver_address.value,
		       context->his_address.length);
		/*
		 * Generate a random sequence number
		 */
		if (*minor_status =
		    krb5_generate_seq_number(&creds.keyblock,
					     &context->my_seq_num)) {
			xfree(context->his_address.contents);
			xfree(context->my_address.contents);
			free((char *)context);
			return(make_gss_re(GSS_RE_FAILURE));
		}
		context->his_seq_num = 0;
		/*
		 * Make a credentials structure
		 */
		memset((char *)&creds, 0, sizeof(creds));
		creds.server = context->him;
		creds.client = context->me;
		/* creds.times.endtime = 0; -- memset 0 takes care of this
					zero means "as long as possible" */
		/* creds.keyblock.keytype = 0; -- as well as this.
					zero means no session keytype
					preference */
		if (*minor_status = krb5_get_credentials(0,
							 ccache,
							 &creds)) {
			krb5_free_cred_contents(&creds);
			free((char *)context);
			return(gss_make_re(GSS_RE_FAILURE));
		}
		/*
		 * Setup the ap_req_options
		 */
		if ((req_flags & GSS_C_MUTUAL_FLAG) ||
		    (req_flags & GSS_C_SEQUENCE_FLAG))
			ap_req_options |= AP_OPTS_MUTUAL_REQUIRED;
		/*
		 * OK, get the authentication header!
		 */
		if (*minor_status = krb5_mk_req_extended(ap_req_options, 0,
						  &creds.times,
						  kdc_options,
						  context->my_seq_num, 0,
						  ccache, &creds, &authent,
						  &outbuf)) {
			memset((char *)&authent, 0, sizeof(authent));
			krb5_free_cred_contents(&creds);
			free((char *)context);
			return(gss_make_re(GSS_RE_FAILURE));	
		}
		context->cusec = authent.cusec;
		context->ctime = authent.ctime;
		memset((char *)&authent, 0, sizeof(authent));
		
		if (*minor_status =
		    krb5_copy_keyblock(&creds.keyblock,
				       &context->session_key)) {
			xfree(outbuf.data);
			krb5_free_cred_contents(&creds);
			free((char *)context);
			return(gss_make_re(GSS_RE_FAILURE));
		}
		
		if (*minor_status = gss_make_token(minor_status,
						   GSS_API_KRB5_TYPE,
						   GSS_API_KRB5_REQ,
						   outbuf.length,
						   outbuf.data,
						   output_token)) {
			xfree(outbuf.data);
			krb5_free_cred_contents(&creds);
			free((char *) context);
			return(gss_make_re(GSS_RE_FAILURE));
		}
		/*
		 * Send over the requested flags information
		 */
		((char *) output_token->value)[4] = context->flags;
		xfree(outbuf.data);
		*context_handle = context;
 		context->state = GSS_KRB_STATE_DOWN;
		*ret_flags = context->flags;
		/*
		 * Don't free server and client because we need them
		 * for the context structure.
		 */
		creds.server = 0;
		creds.client = 0;
		krb5_free_cred_contents(&creds);
		if (ap_req_options & AP_OPTS_MUTUAL_REQUIRED) {
			context->state = GSS_KRB_STATE_MUTWAIT;
			return(GSS_SS_CONTINUE_NEEDED);
		} else {
			context->state = GSS_KRB_STATE_UP;
			return(GSS_S_COMPLETE);
		}
		
	} else {
		context = *context_handle;

		if (context->state != GSS_KRB_STATE_MUTWAIT)
			return(gss_make_re(GSS_RE_FAILURE));
		if (retval = gss_check_token(minor_status, input_token,
					     GSS_API_KRB5_TYPE,
					     GSS_API_KRB5_REP))
			return(retval);
		inbuf.length = input_token->length-4;
		inbuf.data = ((char *)input_token->value)+4;
		
		if (*minor_status = krb5_rd_rep(&inbuf, context->session_key,
						&repl))
			return(gss_make_re(GSS_RE_FAILURE));
		if ((repl->ctime != context->ctime) ||
		    (repl->cusec != context->cusec)) {
			*minor_status = KRB5_SENDAUTH_MUTUAL_FAILED;
			return(gss_make_re(GSS_RE_FAILURE));
		}
		context->his_seq_num = repl->seq_number;
		context->state = GSS_KRB_STATE_UP;
		krb5_free_ap_rep_enc_part(repl);
		return(GSS_S_COMPLETE);
	}
}
