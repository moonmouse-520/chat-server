const grpc = require('@grpc/grpc-js');
const proto_module = require('./proto');
const message_proto = proto_module.message_proto;
const email_module = require('./email');
const const_module = require('./const');
const { v4: uuidv4 } = require('uuid');
const config_module = require('./config');
const redis_module = require('./redis');

async function GetVarifyCode(call, callback) {
    console.log("email is:", call.request.email);
    try {
        let query_res = await redis_module.getRedisCache(const_module.code_prefix + call.request.email);
        console.log("query_res is:", query_res);
        let uniqueId = query_res;
        if (!query_res) {
            uniqueId = uuidv4().replace(/-/g, '').substring(0, 6);
            let bres = await redis_module.setRedisCache(const_module.code_prefix + call.request.email, uniqueId, 180); 
            if(!bres) {
                console.log("setRedisCache failed");
                callback(null, {
                    email: call.request.email, error: const_module.Errors.RedisErr
                });
                return;
            }
        }
        console.log("uniqueId is:", uniqueId);
        let text_str = "您的验证码是：" + uniqueId + "，请在3分钟内使用。";
        //发送邮件
        let mailOptions = {
            from: config_module.email_user, // sender address
            to: call.request.email, // list of receivers
            subject: "验证码", // Subject line
            text: text_str, // plain text body
        };

        let send_result = await email_module.sendEmail(mailOptions);
        if (!send_result) {
            console.log("sendEmail failed");
            callback(null, {
                email: call.request.email, error: const_module.Errors.Exception
            });
            return;
        }
        console.log("send_result is:", send_result);

        callback(null, {
            email: call.request.email, error: const_module.Errors.Success
        });

    } catch (error) {
        console.log("GetVarifyCode error:", error);
        callback(null, {
            email: call.request.email, error: const_module.Errors.Exception
        });
    }

}

function main() {
    var server = new grpc.Server();
    server.addService(message_proto.VarifyService.service, {
        GetVarifyCode: GetVarifyCode
    })
    server.bindAsync('0.0.0.0:50051', grpc.ServerCredentials.createInsecure(), () => {
        console.log('grpc server started on port 50051');
    });
}

main();