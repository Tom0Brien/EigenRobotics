#ifndef RML_INVERSEKINEMATICS_HPP
#define RML_INVERSEKINEMATICS_HPP

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <map>
#include <autodiff/forward/dual.hpp>
#include <autodiff/forward/dual/eigen.hpp>
#include <ifopt/problem.h>
#include <ifopt/ipopt_solver.h>


#include "RobotModel.hpp"
#include "ForwardKinematics.hpp"
#include "Common.hpp"
#include "txml.h"

using namespace autodiff;
using namespace ifopt;

namespace RML {

    /**
     * @brief Defines the variable set for the IK problem, which is the configuration for the robot.
     * @param model The robot model.
     */
    template <typename Scalar>
    class IKVariables : public VariableSet {
        public:

        /**
         * @brief Every variable set has a name, here "var_set1". this allows the constraints
         * and costs to define values and Jacobians specifically w.r.t this variable set.
         */
        IKVariables(const std::string& name, std::shared_ptr<RobotModel<Scalar>> _model, Eigen::Matrix<Scalar, Eigen::Dynamic, 1> q0) : VariableSet(_model->n_q, name)
        {
            // the initial values where the NLP starts iterating from
            q.resize(_model->n_q);
            q = q0;
        }

        /**
         * @brief Overrides the variable set
         */
        void SetVariables(const VectorXd& qin) override
        {
            q = qin;
        };

        /**
         * @brief Gets the configuration vector
         * @return The configuration vector
         */
        VectorXd GetValues() const override
        {
            return q;
        };

        /**
         * @brief Each variable has an upper and lower bound set here
         * @return The bounds
         */
        VecBound GetBounds() const override
        {
            VecBound bounds(GetRows());
            for(int i = 0; i < GetRows(); i++)
            {
                bounds.at(i) = {-M_PI, M_PI};
            }
            return bounds;
        }

        private:
        /// @brief The variable set, which is the robots configuration vector
        Eigen::Matrix<Scalar, Eigen::Dynamic, 1> q;
    };

    /**
     * @brief Defines the constraints for the IK problem
     * @param
     */
    template <typename Scalar>
    class IKConstraint : public ConstraintSet {
        public:
        IKConstraint() : IKConstraint("constraint1") {}

        // This constraint set just contains 1 constraint, however generally
        // each set can contain multiple related constraints.
        IKConstraint(const std::string& name) : ConstraintSet(1, name) {}

        // The constraint value minus the constant value "1", moved to bounds.
        VectorXd GetValues() const override
        {
            VectorXd g(GetRows());
            Eigen::Matrix<Scalar, Eigen::Dynamic, 1> x = GetVariables()->GetComponent("var_set1")->GetValues();
            g(0) = std::pow(x(0),2) + x(1);
            return g;
        };

        // The only constraint in this set is an equality constraint to 1.
        // Constant values should always be put into GetBounds(), not GetValues().
        // For inequality constraints (<,>), use Bounds(x, inf) or Bounds(-inf, x).
        VecBound GetBounds() const override
        {
            VecBound b(GetRows());
            b.at(0) = Bounds(1.0, 1.0);
            return b;
        }

        // This function provides the first derivative of the constraints.
        // In case this is too difficult to write, you can also tell the solvers to
        // approximate the derivatives by finite differences and not overwrite this
        // function, e.g. in ipopt.cc::use_jacobian_approximation_ = true
        // Attention: see the parent class function for important information on sparsity pattern.
        void FillJacobianBlock (std::string var_set, Jacobian& jac_block) const override
        {
            // must fill only that submatrix of the overall Jacobian that relates
            // to this constraint and "var_set1". even if more constraints or variables
            // classes are added, this submatrix will always start at row 0 and column 0,
            // thereby being independent from the overall problem.
            if (var_set == "var_set1") {
                Eigen::Matrix<Scalar, Eigen::Dynamic, 1> x = GetVariables()->GetComponent("var_set1")->GetValues();

                jac_block.coeffRef(0, 0) = 2.0*x(0); // derivative of first constraint w.r.t x0
                jac_block.coeffRef(0, 1) = 1.0;      // derivative of first constraint w.r.t x1
            }
        }
    };

    /**
     * @brief Defines the cost function for the IK problem
     * @param model The robot model.
     * @param source_link_name {s} The link from which the transform is computed.
     * @param target_link_name {t} The link to which the transform is computed.
     * @param desired_pose {d} The desired pose of the target link in the source link frame.
     * @return The cost given a configuration vector.
     */
    template <typename Scalar, typename AutoDiffType>
    class IKCost: public CostTerm {
        public:
        IKCost() : IKCost("IK_cost") {}
        IKCost(const std::string& name,
            std::shared_ptr<RobotModel<AutoDiffType>> _model,
            std::string& _source_link_name,
            std::string& _target_link_name,
            const Eigen::Transform<Scalar, 3, Eigen::Affine>& _desired_pose) : CostTerm(name) {
            model = _model;
            source_link_name = _source_link_name;
            target_link_name = _target_link_name;
            desired_pose = _desired_pose;
        }

        /// @brief The RobotModel used in the IK problem.
        std::shared_ptr<RobotModel<AutoDiffType>> model;

        /// @brief The name of the source link.
        std::string source_link_name;

        /// @brief The name of the target link.
        std::string target_link_name;

        /// @brief The desired pose of the target link in the source link frame.
        Eigen::Transform<Scalar, 3, Eigen::Affine> desired_pose;


        /**
         * @brief The cost function.
         * @param model The robot model.
         * @param q The joint configuration of the robot.
         * @param source_link_name {s} The link from which the transform is computed.
         * @param target_link_name {t} The link to which the transform is computed.
         * @return The configuration vector of the robot model which achieves the desired pose.
         */
        static inline Eigen::Matrix<AutoDiffType, Eigen::Dynamic, Eigen::Dynamic> cost(const Eigen::Matrix<AutoDiffType, Eigen::Dynamic, 1>& q,
                    const std::shared_ptr<RobotModel<AutoDiffType>> model,
                    const std::string& source_link_name,
                    const std::string& target_link_name,
                    const Eigen::Transform<Scalar, 3, Eigen::Affine> desired_pose) {
            // Compute the forward kinematics from the source link to the target link using the current joint angles.
            Eigen::Transform<AutoDiffType, 3, Eigen::Affine> Hst_current = forward_kinematics(model, q, source_link_name, target_link_name);

            // Compute the euler angles for current
            Eigen::Matrix<AutoDiffType, Eigen::Dynamic, Eigen::Dynamic> Rst_current = Hst_current.linear();
            Eigen::Matrix<AutoDiffType, Eigen::Dynamic, 1> Theta_st_current;
            rot2rpy<AutoDiffType>(Rst_current, Theta_st_current);

            // Compute the euler angles for desired
            Eigen::Matrix<AutoDiffType, Eigen::Dynamic, Eigen::Dynamic> Rst_desired = desired_pose.linear();
            Eigen::Matrix<AutoDiffType, Eigen::Dynamic, 1> Theta_st_desired;
            rot2rpy<AutoDiffType>(Rst_desired, Theta_st_desired);

            // Compute R_v_r
            Eigen::Matrix<AutoDiffType, 3, 3> R_v_r = Rst_desired * Rst_current.transpose();
            AutoDiffType orientation_error = (Eigen::Matrix<AutoDiffType, 3, 3>::Identity() - R_v_r).diagonal().sum();
            Eigen::Matrix<AutoDiffType, 1, 1> o_error;
            o_error(0,0) =  orientation_error;
            // Compute axis angle equivalent for R_v_r 
            // Eigen::AngleAxis<double> axis_angle = Eigen::AngleAxis<double>(R_v_r_d);
            // // Compute the error vector
            // using std::sin;
            // Eigen::Matrix<AutoDiffType, 3, 1> error_vector = (axis_angleEigen::Matrix<double, 3, 1>(sin(axis_angle.axis().x()), sin(axis_angle.axis().y()), sin(axis_angle.axis().z()))).template cast<AutoDiffType>();

            // Quadratic cost function q^T*W*q + (k(q) - x*)^TK*(l(q) - x*)))
            Eigen::Matrix<AutoDiffType, Eigen::Dynamic, Eigen::Dynamic> W = Eigen::Matrix<AutoDiffType, Eigen::Dynamic, Eigen::Dynamic>::Identity(model->n_q, model->n_q);
            Eigen::Matrix<AutoDiffType, 3, 3> K = Eigen::Matrix<AutoDiffType, 3, 3>::Identity();
            Eigen::Matrix<AutoDiffType, Eigen::Dynamic, Eigen::Dynamic> cost = ((Hst_current.translation() - desired_pose.translation()).transpose() * 1 * K * (Hst_current.translation() - desired_pose.translation()))
            + (q.transpose() * 1e-6 * W * q)
            + o_error * 50 * o_error; //.transpose()  * 1 * K 
            return cost;
        }

        /**
         * @brief The cost function, which is scalar quadratic cost
         * @return The value of the cost function
         */
        Scalar GetCost() const override
        {
            // Cast q and model to autodiff type
            Eigen::Matrix<AutoDiffType, Eigen::Dynamic, 1> q_auto(GetVariables()->GetComponent("configuration_vector")->GetValues());

            // Evaluate the cost function
            Eigen::Matrix<AutoDiffType, Eigen::Dynamic, Eigen::Dynamic> cost_val = cost(q_auto, model, source_link_name, target_link_name, desired_pose);

            return val(cost_val(0, 0));
        };

        /**
         * @brief Sets the gradient of the cost function given a configuration vector.
         */
        void FillJacobianBlock (std::string var_set, Jacobian& jac) const override
        {
            if (var_set == "configuration_vector") {
                // Cast q and model to autodiff type
                Eigen::Matrix<AutoDiffType, Eigen::Dynamic, 1> q_auto(GetVariables()->GetComponent("configuration_vector")->GetValues());

                // The output vector F = f(x) evaluated together with Jacobian matrix below
                Eigen::Matrix<AutoDiffType, 1, 1> F;

                // Compute the jacobian
                Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> J = jacobian(cost, wrt(q_auto), at(q_auto, model, source_link_name, target_link_name, desired_pose), F);

                // Fill the jacobian block
                jac.resize(J.rows(), J.cols());
                for(int i = 0; i < model->n_q; i++) {
                    jac.coeffRef(0, i) = J(0, i);
                }
            }
        }
    };

    /**
     * @brief Solves the inverse kinematics problem between two links.
     * @param model The robot model.
     * @param source_link_name {s} The link from which the transform is computed.
     * @param target_link_name {t} The link to which the transform is computed.
     * @param desired_pose {d} The desired pose of the target link in the source link frame.
     * @param q0 The initial guess for the configuration vector.
     * @return The configuration vector of the robot model which achieves the desired pose.
     */
    template <typename Scalar>
    Eigen::Matrix<Scalar, Eigen::Dynamic, 1> inverse_kinematics(std::shared_ptr<RobotModel<Scalar>> model,
        std::string& source_link_name,
        std::string& target_link_name,
        const Eigen::Transform<Scalar, 3, Eigen::Affine>& desired_pose,
        Eigen::Matrix<Scalar, Eigen::Dynamic, 1> q0) {

            // Cast model to autodiff type
            std::shared_ptr<RML::RobotModel<autodiff::dual>> autodiff_model;
            autodiff_model = model->template cast<autodiff::dual>();

            // 1. Define the problem
            Problem nlp;
            nlp.AddVariableSet  (std::make_shared<IKVariables<double>>("configuration_vector", model, q0));
            // nlp.AddConstraintSet(std::make_shared<IKConstraint<double>>());
            nlp.AddCostSet      (std::make_shared<IKCost<double, autodiff::dual>>("IK_cost", autodiff_model, source_link_name, target_link_name, desired_pose));

            // nlp.PrintCurrent();

            // 2. Choose solver and options
            IpoptSolver ipopt;
            ipopt.SetOption("linear_solver", "mumps");
            ipopt.SetOption("jacobian_approximation", "exact");
            ipopt.SetOption("max_iter", 250);
            ipopt.SetOption("acceptable_tol", 1e-9);
            // 3. Solve
            ipopt.Solve(nlp);
            Eigen::Matrix<Scalar, Eigen::Dynamic, 1> q = nlp.GetOptVariables()->GetValues();
            return q;
        }
}  //namespace RML

#endif